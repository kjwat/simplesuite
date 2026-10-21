#!/usr/bin/env python3
"""Compare probe output from two viewers, without rescaling either bitmap."""
import argparse
import configparser
from pathlib import Path


def records(directory):
    result = {}
    for path in directory.glob('render-*.ini'):
        record = configparser.ConfigParser(interpolation=None)
        record.read(path)
        render = record['render']
        key = tuple(render.getint(k) for k in ('page', 'rotation', 'width', 'height', 'format', 'stride'))
        result[key] = render
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('first', type=Path, help='first viewer probe directory')
    parser.add_argument('second', type=Path, help='second viewer probe directory')
    args = parser.parse_args()
    first, second = records(args.first), records(args.second)
    common = sorted(first.keys() & second.keys())
    if not common:
        parser.exit(2, 'No captures with matching page, rotation, pixel dimensions and format.\n')
    differences = 0
    for key in common:
        page, rotation, width, height, _, _ = key
        equal = first[key]['sha256'] == second[key]['sha256']
        differences += not equal
        print(f'Page {page + 1}, rotation {rotation}, {width} x {height}: '
              + ('identical backend pixels' if equal else 'DIFFERENT backend pixels'))
    print(f'{len(common)} matched captures, {differences} pixel differences; '
          f'{len(first.keys() ^ second.keys())} unmatched captures.')
    views = []
    for directory in (args.first, args.second):
        record = configparser.ConfigParser(interpolation=None)
        record.read(directory / 'view.ini')
        views.append(record)
    if all('view' in record for record in views):
        for key in ('page', 'rotation', 'scale', 'page_width', 'page_height',
                    'device_scale', 'display_backend', 'cairo_matrix', 'loading',
                    'border_left', 'border_right', 'border_top', 'border_bottom'):
            left, right = (record['view'].get(key, 'unknown') for record in views)
            if left != right:
                print(f'Latest view differs: {key}: {left} / {right}')
        left, right = (dict(record['environment']) if 'environment' in record else {} for record in views)
        for key in sorted(left.keys() | right.keys()):
            if left.get(key) != right.get(key):
                print(f'Launch environment differs: {key}: {left.get(key)} / {right.get(key)}')
    else:
        print('No paired GTK view records; these captures alone do not verify either viewer GUI.')
    print('Backend captures precede GTK painting and compositor output. '
          'Matching backend pixels do not establish identical on-screen appearance.')
    return bool(differences)


if __name__ == '__main__':
    raise SystemExit(main())
