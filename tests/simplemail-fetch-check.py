#!/usr/bin/env python3
"""Exercise delivery and removal against an IMAP model, using real Maildirs."""

from contextlib import ExitStack, redirect_stderr, redirect_stdout
import errno
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import time
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
        self.requests = []
        self.logouts = 0
        self.shutdowns = 0

    def login(self, user, password):
        return "OK", [None]

    def capability(self):
        return "OK", [b"X-GM-EXT-1 UIDPLUS MOVE"]

    def logout(self):
        self.logouts += 1

    def shutdown(self):
        self.shutdowns += 1

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
        self.requests.append((self.selected, command, args))
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

    def body_requests(self):
        return [args[0].split(b",") for _, command, args in self.connection.requests
                if command == "FETCH" and "BODY.PEEK[]" in args[1]]

    def test_empty_check_neither_reads_local_bodies_nor_searches_empty_folders(self):
        archive = self.root / "Archive/cur"
        archive.mkdir(parents=True)
        (archive / "existing-mail").write_bytes(RAW)
        with patch.object(fetch, "fingerprint", side_effect=AssertionError("archive was reread")):
            self.run_receiver()
        self.assertIsNone(self.store.copies)
        self.assertFalse(self.connection.requests)

    def test_bulk_download_uses_two_body_requests_for_forty_messages(self):
        expected = set()
        for uid in range(1, 41):
            raw = RAW.replace(b"mail-1@", f"mail-{uid}@".encode())
            self.connection.add("Inbox", uid, 1000 + uid, raw)
            expected.add(raw)
        result = self.run_receiver()
        self.assertEqual(result.downloaded, 40)
        self.assertEqual(result.removed, 40)
        self.assertEqual(len(self.body_requests()), 2)
        self.assertEqual({path.read_bytes() for path in self.copies()}, expected)

    def test_large_attachments_are_downloaded_with_bounded_batches(self):
        expected = {}
        for uid, megabytes in [(1, 3), (2, 3), (3, 3), (4, 9)]:
            raw = RAW.replace(b"mail-1@", f"mail-{uid}@".encode()).replace(
                b"Message body.", b"x" * (megabytes * 1024 * 1024))
            self.connection.add("Inbox", uid, 1000 + uid, raw)
            expected[str(uid).encode()] = raw
        self.run_receiver()
        self.assertEqual(len(self.body_requests()), 3)
        for batch in self.body_requests():
            self.assertTrue(len(batch) == 1 or sum(len(expected[uid]) for uid in batch) <= 8 * 1024 * 1024)
        self.assertEqual({path.read_bytes() for path in self.copies()}, set(expected.values()))

    def test_out_of_order_batch_responses_keep_the_correct_identity(self):
        expected = set()
        for uid in range(1, 4):
            raw = RAW.replace(b"mail-1@", f"mail-{uid}@".encode())
            self.connection.add("Inbox", uid, 100 + uid, raw)
            expected.add(raw)
        original_uid = self.connection.uid

        def reverse_bodies(command, *args):
            status, data = original_uid(command, *args)
            return (status, list(reversed(data))) if command == "FETCH" and "BODY.PEEK[]" in args[1] else (status, data)

        with patch.object(self.connection, "uid", side_effect=reverse_bodies):
            self.run_receiver()
        self.assertEqual({path.read_bytes() for path in self.copies()}, expected)

    def test_missing_body_in_batch_retains_all_server_copies(self):
        for uid in range(1, 4):
            self.connection.add("Inbox", uid, 100 + uid, RAW.replace(b"mail-1@", f"mail-{uid}@".encode()))
        original_uid = self.connection.uid

        def omit_body(command, *args):
            status, data = original_uid(command, *args)
            if command == "FETCH" and "BODY.PEEK[]" in args[1]:
                data = [item for item in data if not (isinstance(item, tuple) and b"UID 2 " in item[0])]
            return status, data

        with patch.object(self.connection, "uid", side_effect=omit_body):
            with self.assertRaises(fetch.DeliveryError):
                self.run_receiver()
        self.assertFalse(self.copies())
        self.assertFalse(self.connection.mutations)
        self.assertEqual(len(self.connection.messages[FOLDERS["Inbox"]]), 3)

    def test_wrong_identity_in_batch_retains_all_server_copies(self):
        for uid in range(1, 4):
            self.connection.add("Inbox", uid, 100 + uid, RAW.replace(b"mail-1@", f"mail-{uid}@".encode()))
        original_uid = self.connection.uid

        def wrong_identity(command, *args):
            status, data = original_uid(command, *args)
            if command == "FETCH" and "BODY.PEEK[]" in args[1]:
                data = [(item[0].replace(b"X-GM-MSGID 102", b"X-GM-MSGID 999"), item[1])
                        if isinstance(item, tuple) else item for item in data]
            return status, data

        with patch.object(self.connection, "uid", side_effect=wrong_identity):
            with self.assertRaises(fetch.DeliveryError):
                self.run_receiver()
        self.assertFalse(self.copies())
        self.assertFalse(self.connection.mutations)

    def test_warm_fingerprint_cache_does_not_reread_unchanged_mail(self):
        archive = self.root / "Archive/cur"
        archive.mkdir(parents=True)
        (archive / "old-mail").write_bytes(RAW)
        self.store.load_copy_index()
        new_store = fetch.LocalStore(self.root, "test-account", BOXES)
        with patch.object(fetch, "fingerprint", side_effect=AssertionError("cached file was reread")):
            new_store.load_copy_index()
        self.assertEqual(new_store.copies, self.store.copies)

    def test_editing_local_message_invalidates_its_cached_fingerprint(self):
        archive = self.root / "Archive/cur"
        archive.mkdir(parents=True)
        path = archive / "old-mail"
        path.write_bytes(RAW)
        self.store.load_copy_index()
        changed = RAW.replace(b"Message body.", b"Changed body.")
        path.write_bytes(changed)
        new_store = fetch.LocalStore(self.root, "test-account", BOXES)
        new_store.load_copy_index()
        self.assertNotIn(fetch.fingerprint(RAW), new_store.copies)
        self.assertEqual(new_store.copies[fetch.fingerprint(changed)], path)

    def test_cached_fingerprint_cannot_authorize_wrong_message_removal(self):
        archive = self.root / "Archive/cur"
        archive.mkdir(parents=True)
        path = archive / "different-mail"
        different = RAW.replace(b"Message body.", b"Changed body.")
        path.write_bytes(different)
        self.store.load_copy_index()
        cache = json.loads(self.store.index_path.read_text())
        cache["entries"] = {identity: fetch.fingerprint(RAW) for identity in cache["entries"]}
        self.store.index_path.write_text(json.dumps(cache))
        self.store = fetch.LocalStore(self.root, "test-account", BOXES)
        self.connection.store = self.store
        self.connection.add("Inbox", 1, 101)
        self.run_receiver()
        self.assertEqual({copy.read_bytes() for copy in self.copies()}, {RAW, different})

    def test_broken_or_unwritable_speed_cache_does_not_block_delivery(self):
        self.store.index_path.mkdir()
        self.connection.add("Inbox", 1, 101)
        self.run_receiver()
        self.assertEqual(self.copies()[0].read_bytes(), RAW)
        self.assertTrue(all(not messages for messages in self.connection.messages.values()))

    def reconnect_fixture(self):
        self.account = {"host": "imap.example.test", "user": "user@example.test", "pass": "private-fixture-password"}
        account_id = fetch.hashlib.sha256((self.account["host"] + "\0" + self.account["user"]).encode()).hexdigest()[:20]
        self.store = fetch.LocalStore(self.root, account_id, BOXES)
        first = FakeIMAP(self.store)
        second = FakeIMAP(self.store)
        second.messages = first.messages
        return first, second

    def main_fixture(self, connections):
        stack = ExitStack()
        stack.enter_context(patch.object(fetch, "read_account", return_value=self.account))
        stack.enter_context(patch.object(fetch, "read_config", return_value={"maildir": str(self.root)}))
        self.ssl_factory = stack.enter_context(patch.object(fetch.imaplib, "IMAP4_SSL", side_effect=connections))
        self.retry_sleep = stack.enter_context(patch.object(fetch.time, "sleep"))
        stack.enter_context(redirect_stdout(io.StringIO()))
        self.diagnostics = stack.enter_context(redirect_stderr(io.StringIO()))
        return stack

    def test_connection_reset_reconnects_and_delivers_complete_mail(self):
        first, second = self.reconnect_fixture()
        first.add("Inbox", 1, 101)
        with self.main_fixture([first, second]), patch.object(first, "select", side_effect=ConnectionResetError("private server response")):
            self.assertEqual(fetch.main(["--remove-server-copy"]), 0)
        self.assertEqual(self.ssl_factory.call_count, 2)
        self.assertEqual((first.shutdowns, second.logouts), (1, 1))
        self.retry_sleep.assert_called_once_with(0.5)
        self.assertNotIn("private server response", self.diagnostics.getvalue())
        self.assertEqual(self.copies()[0].read_bytes(), RAW)
        self.assertTrue(all(not messages for messages in first.messages.values()))

    def test_disconnect_after_move_resumes_cleanup_without_duplicate_download(self):
        first, second = self.reconnect_fixture()
        first.add("Inbox", 1, 101)
        original = first.uid

        def disconnect_after_move(command, *args):
            if command == "SEARCH" and args[1] == "UID":
                raise fetch.imaplib.IMAP4.abort("private server response")
            return original(command, *args)

        with self.main_fixture([first, second]), patch.object(first, "uid", side_effect=disconnect_after_move):
            self.assertEqual(fetch.main(["--remove-server-copy"]), 0)
        self.assertEqual(len(self.copies()), 1)
        self.assertEqual(self.copies()[0].read_bytes(), RAW)
        self.assertFalse(any(command == "FETCH" and "BODY.PEEK[]" in args[1]
                             for _, command, args in second.requests))
        self.assertTrue(all(not messages for messages in first.messages.values()))

    def test_temporary_move_failure_retries_with_the_saved_receipt(self):
        first, second = self.reconnect_fixture()
        first.add("Inbox", 1, 101)
        first.fail_move = True
        with self.main_fixture([first, second]):
            self.assertEqual(fetch.main(["--remove-server-copy"]), 0)
        self.assertEqual(len(self.copies()), 1)
        self.assertFalse(any(command == "FETCH" and "BODY.PEEK[]" in args[1]
                             for _, command, args in second.requests))
        self.assertTrue(all(not messages for messages in first.messages.values()))

    def test_persistent_disconnect_stops_after_one_retry_and_retains_server_mail(self):
        first, second = self.reconnect_fixture()
        first.add("Inbox", 1, 101)
        with self.main_fixture([first, second]), \
             patch.object(first, "select", side_effect=fetch.imaplib.IMAP4.abort("private")), \
             patch.object(second, "select", side_effect=fetch.imaplib.IMAP4.abort("private")):
            with self.assertRaises(fetch.imaplib.IMAP4.abort):
                fetch.main(["--remove-server-copy"])
        self.assertEqual(self.ssl_factory.call_count, 2)
        self.assertEqual((first.shutdowns, second.shutdowns), (1, 1))
        self.assertFalse(self.copies())
        self.assertIn(b"1", first.messages[FOLDERS["Inbox"]])

    def test_bad_password_is_not_retried_or_exposed(self):
        first, second = self.reconnect_fixture()
        with self.main_fixture([first, second]), \
             patch.object(first, "login", side_effect=fetch.imaplib.IMAP4.error(self.account["pass"])):
            with self.assertRaises(fetch.DeliveryError) as failure:
                fetch.main(["--remove-server-copy"])
        self.assertEqual(self.ssl_factory.call_count, 1)
        self.assertIn("login failed", str(failure.exception))
        self.assertNotIn(self.account["pass"], str(failure.exception))
        self.retry_sleep.assert_not_called()

    def test_disk_failure_does_not_reconnect_or_remove_server_mail(self):
        first, second = self.reconnect_fixture()
        first.add("Inbox", 1, 101)
        with self.main_fixture([first, second]), \
             patch.object(fetch.os, "fsync", side_effect=OSError(errno.ENOSPC, "No space left on device")):
            with self.assertRaises(OSError):
                fetch.main(["--remove-server-copy"])
        self.assertEqual(self.ssl_factory.call_count, 1)
        self.assertFalse(first.mutations)
        self.assertIn(b"1", first.messages[FOLDERS["Inbox"]])

    def test_busy_delivery_lock_prevents_another_login(self):
        first, second = self.reconnect_fixture()
        with self.main_fixture([first, second]), fetch.delivery_lock(self.root):
            with self.assertRaises(fetch.DeliveryError):
                fetch.main(["--remove-server-copy"])
        self.ssl_factory.assert_not_called()

    def test_error_details_do_not_expose_protocol_responses(self):
        for error in [fetch.imaplib.IMAP4.abort("private response"), fetch.imaplib.IMAP4.error("private response"),
                      ConnectionResetError("private response"), TimeoutError("private response")]:
            self.assertNotIn("private response", fetch.error_detail(error))

    def test_live_notification_is_emitted_before_server_cleanup(self):
        self.connection.add("Inbox", 1, 101)
        notifications = []
        original = self.connection.uid

        def notified():
            self.assertTrue(self.store.has_receipt("101"))
            self.assertEqual(self.copies()[0].read_bytes(), RAW)
            self.assertFalse(self.connection.mutations)
            notifications.append(True)

        def check_before_move(command, *args):
            if command == "MOVE":
                self.assertEqual(notifications, [True])
            return original(command, *args)

        with patch.object(self.connection, "uid", side_effect=check_before_move):
            fetch.Receiver(self.connection, self.store, FOLDERS, on_delivery=notified, quiet=True).run()
        self.assertEqual(notifications, [True])

    def test_live_arrival_uses_existing_login_and_only_inbox_and_trash(self):
        first, second = self.reconnect_fixture()
        control = fetch.WatchControl()
        calls = []

        def idle_arrival(connection, controller, seconds):
            self.assertIs(connection, first)
            calls.append(True)
            if len(calls) == 1:
                first.requests.clear()
                first.add("Inbox", 1, 101)
                return "arrival"
            raise fetch.WatchStopped

        with self.main_fixture([first, second]), \
             patch.object(first, "capability", return_value=("OK", [b"X-GM-EXT-1 UIDPLUS MOVE IDLE"])), \
             patch.object(fetch, "idle_wait", side_effect=idle_arrival), \
             patch.object(fetch, "watch_event") as events:
            self.assertEqual(fetch.main(["--remove-server-copy", "--watch"]), 0)
        self.assertEqual(self.ssl_factory.call_count, 1)
        self.assertEqual(len(self.copies()), 1)
        self.assertEqual(self.copies()[0].read_bytes(), RAW)
        self.assertTrue(all(folder in (FOLDERS["Inbox"], FOLDERS["Trash"])
                            for folder, _, _ in first.requests))
        self.assertIn(unittest.mock.call("MAIL"), events.call_args_list)
        self.assertTrue(all(not messages for messages in first.messages.values()))

    def test_live_manual_request_collects_sent_mail_without_waiting_for_timer(self):
        first, second = self.reconnect_fixture()
        control = fetch.WatchControl()
        calls = []

        def request_check(connection, controller, seconds):
            calls.append(True)
            if len(calls) == 1:
                first.add("Sent", 1, 101)
                return "check"
            raise fetch.WatchStopped

        with patch.object(fetch, "idle_wait", side_effect=request_check), \
             patch.object(fetch, "watch_event"):
            with self.assertRaises(fetch.WatchStopped):
                fetch.watch_connection(first, {b"IDLE"}, FOLDERS, self.root,
                                       self.store.account_id, BOXES, True, control, interval=300)
        self.assertEqual(len(self.copies()), 1)
        self.assertEqual(self.copies()[0].parent.parent.name, "Sent")
        self.assertTrue(all(not messages for messages in first.messages.values()))

    def test_live_disconnect_reconnects_and_never_exposes_server_response(self):
        first, second = self.reconnect_fixture()
        with self.main_fixture([first, second]), \
             patch.object(fetch, "watch_connection", side_effect=[fetch.imaplib.IMAP4.abort("private response"), fetch.WatchStopped]), \
             patch.object(fetch.WatchControl, "wait"):
            self.assertEqual(fetch.main(["--remove-server-copy", "--watch"]), 0)
        self.assertEqual(self.ssl_factory.call_count, 2)
        self.assertEqual((first.shutdowns, second.shutdowns), (1, 1))
        self.assertNotIn("private response", self.diagnostics.getvalue())

    def test_repeated_live_failures_back_off_instead_of_logging_in_continuously(self):
        first, second = self.reconnect_fixture()
        third = FakeIMAP(self.store)
        with self.main_fixture([first, second, third]), \
             patch.object(fetch, "watch_connection", side_effect=[fetch.imaplib.IMAP4.abort("private"),
                                                                  fetch.imaplib.IMAP4.abort("private"), fetch.WatchStopped]), \
             patch.object(fetch.WatchControl, "wait") as wait:
            self.assertEqual(fetch.main(["--remove-server-copy", "--watch"]), 0)
        self.assertEqual(wait.call_args_list, [unittest.mock.call(1), unittest.mock.call(2)])

    def test_failed_local_save_emits_no_live_arrival(self):
        self.connection.add("Inbox", 1, 101)
        with patch.object(fetch.os, "fsync", side_effect=OSError("disk failure")), \
             patch.object(fetch, "watch_event") as events:
            with self.assertRaises(OSError):
                fetch.watch_connection(self.connection, {b"IDLE"}, FOLDERS, self.root,
                                       self.store.account_id, BOXES, True, fetch.WatchControl())
        self.assertFalse(events.called)
        self.assertFalse(self.connection.mutations)


class IdleProtocolChecks(unittest.TestCase):
    def connection(self, arrival):
        client, server = socket.socketpair()
        client.settimeout(2)
        seen = []
        failures = []

        def serve():
            try:
                with server.makefile("rb") as stream:
                    server.sendall(b"* OK fixture ready\r\n")
                    idle_tag = None
                    for line in stream:
                        seen.append(line.strip())
                        if line == b"DONE\r\n":
                            server.sendall(idle_tag + b" OK IDLE ended\r\n")
                            idle_tag = None
                            continue
                        tag, command = line.rstrip(b"\r\n").split(b" ", 1)
                        verb = command.split(b" ", 1)[0]
                        if verb == b"CAPABILITY":
                            server.sendall(b"* CAPABILITY IMAP4rev1 IDLE\r\n" + tag + b" OK capabilities\r\n")
                        elif verb == b"SELECT":
                            server.sendall(b"* 0 EXISTS\r\n" + tag + b" OK selected\r\n")
                        elif verb == b"IDLE":
                            idle_tag = tag
                            if arrival == "before":
                                server.sendall(b"* 1 EXISTS\r\n+ idling\r\n")
                            elif arrival == "after":
                                server.sendall(b"+ idling\r\n* 1 EXISTS\r\n")
                            elif arrival == "disconnect":
                                server.sendall(b"+ idling\r\n")
                                return
                            else:
                                server.sendall(b"+ idling\r\n")
                        elif verb == b"UID":
                            self.assertIsNone(idle_tag, "Command sent before DONE")
                            server.sendall(b"* SEARCH 77\r\n" + tag + b" OK search\r\n")
                        elif verb == b"LOGOUT":
                            server.sendall(b"* BYE fixture logout\r\n" + tag + b" OK logout\r\n")
                            return
                        else:
                            server.sendall(tag + b" OK done\r\n")
            except (OSError, ValueError) as error:
                failures.append(error)
            finally:
                server.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()

        class PairIMAP(fetch.imaplib.IMAP4):
            def open(self, host="", port=143, timeout=None):
                self.sock = client
                self.file = client.makefile("rb")

        connection = PairIMAP()
        connection.login("fixture", "fixture")
        connection.select("INBOX")

        def cleanup():
            try:
                connection.shutdown()
            except OSError:
                pass
            thread.join(2)
            self.assertFalse(thread.is_alive(), "IDLE fixture did not stop")
            self.assertFalse(failures)

        self.addCleanup(cleanup)
        return connection, seen

    def test_buffered_notifications_before_and_after_continuation_wake_immediately(self):
        for arrival in ("before", "after"):
            with self.subTest(arrival=arrival):
                connection, seen = self.connection(arrival)
                started = time.monotonic()
                self.assertEqual(fetch.idle_wait(connection, fetch.WatchControl(), 10), "arrival")
                self.assertLess(time.monotonic() - started, 0.5)
                self.assertEqual(fetch.uid_search(connection, "ALL"), [b"77"])
                self.assertIn(b"DONE", seen)

    def test_control_request_ends_idle_before_checking(self):
        connection, seen = self.connection(None)
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        self.addCleanup(os.close, write_fd)
        os.write(write_fd, b"c")
        self.assertEqual(fetch.idle_wait(connection, fetch.WatchControl(read_fd), 10), "check")
        self.assertEqual(fetch.uid_search(connection, "ALL"), [b"77"])
        self.assertIn(b"DONE", seen)

    def test_control_eof_stops_without_expunging(self):
        connection, seen = self.connection(None)
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        os.close(write_fd)
        with self.assertRaises(fetch.WatchStopped):
            fetch.idle_wait(connection, fetch.WatchControl(read_fd), 10)
        self.assertIn(b"DONE", seen)
        self.assertFalse(any(b"EXPUNGE" in command or b"CLOSE" in command for command in seen))

    def test_idle_disconnect_is_a_connection_failure(self):
        connection, _ = self.connection("disconnect")
        with self.assertRaises((fetch.imaplib.IMAP4.abort, OSError)):
            fetch.idle_wait(connection, fetch.WatchControl(), 10)


if __name__ == "__main__":
    unittest.main()
