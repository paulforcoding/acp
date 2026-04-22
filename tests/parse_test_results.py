#!/usr/bin/env python3
"""Parse Catch2 JSON reporter output and summarize test results."""

import json
import sys

def parse_catch2_json(filepath):
    """Catch2 JSON reporter outputs a single large JSON object."""
    with open(filepath, 'r') as f:
        data = json.load(f)

    tests = []
    totals = None

    if 'test-run' in data:
        tr = data['test-run']
        # Catch2 groups tests into sections
        for section in tr.get('test-cases', []):
            tests.append(section)
        totals = tr.get('totals', {})

    return tests, totals

def extract_failed(tests):
    failed = []
    for tc in tests:
        info = tc.get('test-info', {})
        name = info.get('name', 'unknown')
        filename = info.get('source-location', {}).get('filename', 'unknown')
        line = info.get('source-location', {}).get('line', 0)
        tags = info.get('tags', [])

        assertions = tc.get('totals', {}).get('assertions', {})
        if assertions.get('failed', 0) > 0:
            # Extract failed assertions
            failures = []
            for section in tc.get('test-cases', []):
                for assertion in section.get('assertions', []):
                    if assertion.get('status') == False:
                        msg = assertion.get('msg', '')
                        exp = assertion.get('expansion', '')
                        failures.append({'msg': msg, 'expansion': exp})

            failed.append({
                'name': name,
                'filename': filename,
                'line': line,
                'tags': tags,
                'failed_assertions': failures,
            })
    return failed

def main():
    filepath = sys.argv[1] if len(sys.argv) > 1 else '/tmp/acp_test_stdout.jsonl'
    tests, totals = parse_catch2_json(filepath)

    print("=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)

    if totals:
        tc = totals.get('test-cases', {})
        ast = totals.get('assertions', {})
        print(f"Test cases:  passed={tc.get('passed',0)}  failed={tc.get('failed',0)}  skipped={tc.get('skipped',0)}")
        print(f"Assertions:  passed={ast.get('passed',0)}  failed={ast.get('failed',0)}  skipped={ast.get('skipped',0)}")
    else:
        print("No totals found.")

    failed = extract_failed(tests)
    if failed:
        print(f"\nFAILED TESTS ({len(failed)})")
        print("-" * 60)
        for i, f in enumerate(failed, 1):
            print(f"\n{i}. {f['name']}")
            print(f"   File: {f['filename']}:{f['line']}")
            print(f"   Tags: {', '.join(f['tags'])}")
            for a in f['failed_assertions']:
                print(f"   Assertion: {a['msg']}")
                if a['expansion']:
                    print(f"   Expansion: {a['expansion']}")
    else:
        print("\nAll tests passed!")

if __name__ == '__main__':
    main()
