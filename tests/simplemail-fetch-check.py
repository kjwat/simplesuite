#!/usr/bin/env python3
"""Exercise delivery and removal against an IMAP model, using real Maildirs."""

from contextlib import redirect_stdout
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

loader = importlib.machinery.SourceFileLoader("simplemail_fetch", str(Path(__file__).resolve().parents[1] / "simplemail-fetch"))
spec = importlib.util.spec_from_loader(loader.name, loader)
fetch = importlib.util.module_from_spec(spec)
loader.exec_module(fetch)

FOLDERS = {"Inbox": b"INBOX", "Sent": b"[Gmail]/Sent Mail", "Drafts": b"[Gmail]/Drafts",
           "Archive": b"[Gmail]/All Mail", "Spam": b"[Gmail]/Spam", "Trash": b"[Gmail]/Trash"}
BOXES = {role: role for role in FOLDERS}
RAW = (b"From: sender@example.test\r\nTo: recipient@example.test\r\n"
       b"Subject: Original message\r\nMessage-ID: <mail-1@example.test>\r\n"
       b"Date: Fri, 2 Oct 2026 10:00:00 +0000\r\nMIME-Version: 1.0\r\n"
       b"Content-Type: multipart/mixed; boundary=parts\r\n\r\n"
       b"--parts\r\nContent-Type: text/plain\r\n\r\nMessage body.\r\n"
       b"--parts\r\nContent-Type: application/octet-stream\r\n"
       b"Content-Transfer-Encoding: base64\r\n\r\nYXR0YWNobWVudA==\r\n--parts--\r\n")


class FakeIMAP:
    def __init__(self, store):
        self.store = store
        self.messages = {name: {} for name in FOLDERS.values()}
        self.selected = None
        self.mutations = []
        self.fail_move = False
        self.fail_expunge = False
        self.truncated = False
        self.next_uid = 100

    def add(self, role, uid, message_id, body=RAW):
        self.messages[FOLDERS[role]][str(uid).encode()] = (str(message_id), body)

    def select(self, name, readonly=False):
        self.selected = name.strip(b'"')
        return "OK", [str(len(self.messages[self.selected])).encode()]

    def list(self):
        return "OK", [b'(\\HasNoChildren) "/" "INBOX"',
                      b'(\\All) "/" "[Gmail]/All Mail"',
                      b'(\\Sent) "/" "[Gmail]/Sent Mail"',
                      b'(\\Drafts) "/" "[Gmail]/Drafts"',
                      b'(\\Junk) "/" "[Gmail]/Spam"',
                      b'(\\Trash) "/" "[Gmail]/Trash"']

    def uid(self, command, *args):
        messages = self.messages[self.selected]
        if command == "SEARCH":
            if args[1] == "ALL":
                return "OK", [b" ".join(messages)]
            return "OK", [b" ".join(uid for uid in args[2].split(b",") if uid in messages)]
        uids = args[0].split(b",")
        if command == "FETCH":
            response = []
            for uid in uids:
                if uid not in messages:
                    continue
                message_id, raw = messages[uid]
                head = (b"1 (UID " + uid + b" X-GM-MSGID " + message_id.encode()
                        + b' X-GM-LABELS (\\Inbox) FLAGS () INTERNALDATE "02-Oct-2026 10:00:00 +0000"'
                        + b" RFC822.SIZE " + str(len(raw)).encode())
                if "BODY.PEEK[]" in args[1]:
                    response += [(head + b" BODY[] {" + str(len(raw)).encode() + b"}", raw[:-1] if self.truncated else raw), b")"]
                else:
                    response.append(head + b")")
            return "OK", response
        # Every destructive operation must be preceded by a durable receipt.
        for uid in uids:
            if uid in messages:
                assert self.store.has_receipt(messages[uid][0])
        if command == "MOVE" and self.fail_move:
            return "NO", [b"temporary failure"]
        if command == "EXPUNGE" and self.fail_expunge:
            return "NO", [b"temporary failure"]
        self.mutations.append((command, tuple(uids)))
        if command == "MOVE":
            for uid in uids:
                if uid in messages:
                    message = messages.pop(uid)
                    for folder in self.messages.values():
                        for other_uid, other in list(folder.items()):
                            if other[0] == message[0]:
                                del folder[other_uid]
                    self.next_uid += 1
                    self.messages[FOLDERS["Trash"]][str(self.next_uid).encode()] = message
        elif command == "EXPUNGE":
            for uid in uids:
                messages.pop(uid, None)
        elif command != "STORE":
            raise AssertionError(command)
        return "OK", [None]


class DeliveryChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = fetch.LocalStore(self.root, "test-account", BOXES)
        self.connection = FakeIMAP(self.store)

    def tearDown(self):
        self.temp.cleanup()

    def run_receiver(self):
        with redirect_stdout(io.StringIO()):
            receiver = fetch.Receiver(self.connection, self.store, FOLDERS)
            receiver.run()
            return receiver

    def copies(self):
        return list(self.root.glob("*/cur/*")) + list(self.root.glob("*/new/*"))

    def test_complete_mime_is_saved_before_targeted_removal(self):
        self.connection.add("Inbox", 1, 101)
        self.connection.add("Archive", 22, 101)  # Gmail labels refer to one message.
        result = self.run_receiver()
        self.assertEqual(result.downloaded, 1)
        self.assertEqual(result.removed, 1)
        self.assertEqual(self.copies()[0].read_bytes(), RAW)
        self.assertEqual(self.copies()[0].stat().st_mode & 0o777, 0o600)
        self.assertTrue(all(not messages for messages in self.connection.messages.values()))
        self.assertTrue(any(command == "EXPUNGE" for command, _ in self.connection.mutations))

    def test_disk_failure_retains_server_message(self):
        self.connection.add("Inbox", 1, 101)
        with patch.object(fetch.os, "fsync", side_effect=OSError("disk failure")):
            with self.assertRaises(OSError):
                self.run_receiver()
        self.assertFalse(self.store.has_receipt("101"))
        self.assertFalse(self.connection.mutations)
        self.assertIn(b"1", self.connection.messages[FOLDERS["Inbox"]])

    def test_receipt_failure_retains_server_message_and_retry_deduplicates(self):
        self.connection.add("Inbox", 1, 101)
        with patch.object(self.store, "write_receipt", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.run_receiver()
        self.assertEqual(len(self.copies()), 1)
        self.assertFalse(self.connection.mutations)
        self.store = fetch.LocalStore(self.root, "test-account", BOXES)
        self.connection.store = self.store
        self.run_receiver()
        self.assertEqual(len(self.copies()), 1)

    def test_partial_body_retains_server_message(self):
        self.connection.add("Inbox", 1, 101)
        self.connection.truncated = True
        with self.assertRaises(fetch.DeliveryError):
            self.run_receiver()
        self.assertFalse(self.copies())
        self.assertFalse(self.connection.mutations)

    def test_move_failure_retry_preserves_local_folder_and_read_state(self):
        self.connection.add("Inbox", 1, 101)
        self.connection.fail_move = True
        with self.assertRaises(fetch.DeliveryError):
            self.run_receiver()
        archive = self.root / "Archive" / "cur"
        archive.mkdir(parents=True)
        moved = archive / ("moved-" + self.copies()[0].name + "S")
        self.copies()[0].rename(moved)
        self.connection.fail_move = False
        self.store = fetch.LocalStore(self.root, "test-account", BOXES)
        self.connection.store = self.store
        self.run_receiver()
        self.assertEqual(self.copies(), [moved])

    def test_retry_respects_intentional_local_deletion(self):
        self.connection.add("Inbox", 1, 101)
        self.connection.fail_move = True
        with self.assertRaises(fetch.DeliveryError):
            self.run_receiver()
        self.copies()[0].unlink()
        self.connection.fail_move = False
        self.run_receiver()
        self.assertFalse(self.copies())
        self.assertTrue(all(not messages for messages in self.connection.messages.values()))

    def test_mbsync_copy_is_reused_in_its_current_local_folder(self):
        trash = self.root / "Trash" / "cur"
        trash.mkdir(parents=True)
        path = trash / "user-moved-message,U=9:2,S"
        path.write_bytes(b"X-TUID: from-mbsync\n" + RAW.replace(b"\r\n", b"\n"))
        self.store = fetch.LocalStore(self.root, "test-account", BOXES)
        self.connection.store = self.store
        self.connection.add("Inbox", 1, 101)
        result = self.run_receiver()
        self.assertEqual(result.downloaded, 0)
        self.assertEqual(self.copies(), [path])

    def test_same_message_id_with_different_body_is_saved(self):
        archive = self.root / "Archive" / "cur"
        archive.mkdir(parents=True)
        (archive / "old-message").write_bytes(RAW.replace(b"Message body.", b"Different body."))
        self.store = fetch.LocalStore(self.root, "test-account", BOXES)
        self.connection.store = self.store
        self.connection.add("Inbox", 1, 101)
        self.run_receiver()
        self.assertEqual(len(self.copies()), 2)

    def test_outgoing_copy_survives_server_sent_cleanup(self):
        sent = self.root / "Sent" / "cur"
        sent.mkdir(parents=True)
        (sent / "local-sent:2,S").write_bytes(RAW)
        self.store = fetch.LocalStore(self.root, "test-account", BOXES)
        self.connection.store = self.store
        self.connection.add("Sent", 8, 108, b"Received: by SMTP\r\n" + RAW)
        self.run_receiver()
        self.assertEqual(len(self.copies()), 1)
        self.assertEqual(self.copies()[0].parent, sent)

    def test_corrupt_receipt_retains_server_message(self):
        self.connection.add("Inbox", 1, 101)
        (self.store.receipts / "101.json").write_text(json.dumps({"version": 1, "gmail_id": "101"}))
        with self.assertRaises(fetch.DeliveryError):
            self.run_receiver()
        self.assertFalse(self.connection.mutations)

    def test_expunge_failure_retry_does_not_duplicate_saved_message(self):
        self.connection.add("Inbox", 1, 101)
        self.connection.fail_expunge = True
        with self.assertRaises(fetch.DeliveryError):
            self.run_receiver()
        self.connection.fail_expunge = False
        self.run_receiver()
        self.assertEqual(len(self.copies()), 1)
        self.assertTrue(all(not messages for messages in self.connection.messages.values()))

    def test_other_trash_mail_is_downloaded_before_expunging(self):
        self.connection.add("Inbox", 1, 101)
        self.connection.add("Trash", 7, 107, RAW.replace(b"mail-1@", b"mail-7@"))
        self.run_receiver()
        self.assertEqual(len(self.copies()), 2)
        self.assertEqual({path.parent.parent.name for path in self.copies()}, {"Inbox", "Trash"})

    def test_localized_special_folders_are_discovered(self):
        self.assertEqual(fetch.discover_folders(self.connection), FOLDERS)
        with patch.object(self.connection, "list", return_value=("OK", [b'(\\Trash) "/" "[Gmail]/Bin"'] + self.connection.list()[1][:-1])):
            self.assertEqual(fetch.discover_folders(self.connection)["Trash"], b"[Gmail]/Bin")

    def test_two_receivers_cannot_run_together(self):
        with fetch.delivery_lock(self.root):
            with self.assertRaises(fetch.DeliveryError):
                with fetch.delivery_lock(self.root):
                    self.fail("second receiver acquired the lock")

    def test_download_only_saves_without_server_mutations(self):
        self.connection.add("Inbox", 1, 101)
        with redirect_stdout(io.StringIO()):
            fetch.Receiver(self.connection, self.store, FOLDERS, remove_server_copy=False).run()
        self.assertEqual(self.copies()[0].read_bytes(), RAW)
        self.assertFalse(self.connection.mutations)
        self.assertIn(b"1", self.connection.messages[FOLDERS["Inbox"]])

    def test_unseen_trash_arrival_is_not_expunged_with_saved_uids(self):
        self.connection.add("Trash", 1, 101)
        original_uid = self.connection.uid

        def add_arrival(command, *args):
            if command == "STORE":
                self.connection.add("Trash", 2, 102, RAW.replace(b"mail-1@", b"mail-2@"))
            return original_uid(command, *args)

        with patch.object(self.connection, "uid", side_effect=add_arrival):
            self.run_receiver()
        self.assertIn(b"2", self.connection.messages[FOLDERS["Trash"]])
        self.assertFalse(self.store.has_receipt("102"))

    def test_no_removal_option_is_an_error(self):
        with patch.object(fetch, "read_account") as credentials:
            with redirect_stdout(io.StringIO()), patch("sys.stderr", new=io.StringIO()):
                with self.assertRaises(SystemExit):
                    fetch.main([])
            credentials.assert_not_called()

    def test_corrupt_saved_copy_blocks_removal(self):
        self.connection.add("Inbox", 1, 101)
        with redirect_stdout(io.StringIO()):
            fetch.Receiver(self.connection, self.store, FOLDERS, remove_server_copy=False).run()
        self.copies()[0].write_bytes(b"damaged local copy")
        with self.assertRaises(fetch.DeliveryError):
            self.run_receiver()
        self.assertFalse(self.connection.mutations)

    def test_original_labels_are_retained_before_trash_removal(self):
        self.connection.add("Inbox", 1, 101)
        self.run_receiver()
        record = json.loads((self.store.receipts / "101.json").read_text())
        self.assertIn("X-GM-LABELS (\\Inbox)", record["gmail_metadata"])


if __name__ == "__main__":
    unittest.main()
