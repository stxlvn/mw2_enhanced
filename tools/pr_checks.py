"""Checks a pull request can pass without the game: python -I tools/pr_checks.py [--base <git rev>]
Run by .github/workflows/pr-checks.yml and fine to run locally from the repository root.

What it checks (a failure exits 1):
  1. patches    every content/patches/**/*.patch hunk is well formed: `@@@ replace` .. `@@@ with` .. `@@@ end`,
                `@@@ subst <function>` .. `@@@ to` .. `@@@ end`
  2. scripts    content/scripts/**/*.gsc: braces, brackets and parentheses balance (strings and comments skipped)
  3. files      no game binaries or assets (.ff .iwi .exe .zone .ogg .wav ...) and nothing over 20 MB is tracked
  4. privacy    no Windows user folder (C:\\Users\\<name>) or /home/<name> path in a tracked text file
  5. changelog  (warning only, needs --base) code or content changed but CHANGELOG.md / CHANGELOG.ru.md did not
What it does NOT check: that a patch applies to the game's scripts (tools/check_patches.py, needs the extracted game
scripts, run it locally) and that the mod runs.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

errors = []
warnings = []


def fail(path, line, msg):
    errors.append((path, line, msg))
    print('::error file=%s,line=%d::%s' % (path, line, msg))


def warn(msg):
    warnings.append(msg)
    print('::warning::%s' % msg)


def git(*args):
    return subprocess.run(['git'] + list(args), capture_output=True, text=True, check=True).stdout


def tracked():
    return [f for f in git('ls-files').split('\n') if f]


def read(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        return f.read()


# 1. patches ---------------------------------------------------------------------------------------------------------
def check_patches(files):
    for path in files:
        if not path.startswith('content/patches/') or not path.endswith('.patch'):
            continue
        state = None  # None between hunks; 'replace', 'with', 'subst', 'to' inside one
        start = 0
        for n, line in enumerate(read(path).replace('\r\n', '\n').split('\n'), 1):
            if not line.startswith('@@@ ') and line != '@@@':
                continue
            word = line[4:].split(' ')[0].strip()
            if word in ('replace', 'subst'):
                if state is not None:
                    fail(path, n, 'hunk opened at line %d is not closed with "@@@ end"' % start)
                state, start = word, n
                if word == 'subst' and len(line.split()) < 3:
                    fail(path, n, '"@@@ subst" needs a function name')
            elif word == 'with':
                if state != 'replace':
                    fail(path, n, '"@@@ with" without an open "@@@ replace"')
                state = 'with'
            elif word == 'to':
                if state != 'subst':
                    fail(path, n, '"@@@ to" without an open "@@@ subst"')
                state = 'to'
            elif word == 'end':
                if state not in ('with', 'to'):
                    fail(path, n, '"@@@ end" without a complete hunk (replace..with / subst..to)')
                state = None
            else:
                fail(path, n, 'unknown patch directive "%s"' % line)
        if state is not None:
            fail(path, start, 'hunk opened here is never closed with "@@@ end"')


# 2. scripts ---------------------------------------------------------------------------------------------------------
PAIRS = {')': '(', ']': '[', '}': '{'}


def check_gsc(files):
    for path in files:
        if not path.startswith('content/scripts/') or not path.endswith('.gsc'):
            continue
        text = read(path)
        stack = []
        i, n, line = 0, len(text), 1
        while i < n:
            c = text[i]
            if c == '\n':
                line += 1
            elif text.startswith('//', i):
                j = text.find('\n', i)
                i = n if j < 0 else j
                continue
            elif text.startswith('/*', i):
                j = text.find('*/', i + 2)
                j = n if j < 0 else j + 2
                line += text.count('\n', i, j)
                i = j
                continue
            elif c == '"':
                j = i + 1
                while j < n and text[j] != '"' and text[j] != '\n':
                    j += 2 if text[j] == '\\' else 1
                i = j + 1
                continue
            elif c in '([{':
                stack.append((c, line))
            elif c in PAIRS:
                if not stack or stack[-1][0] != PAIRS[c]:
                    fail(path, line, 'unbalanced "%s"' % c)
                    stack = []
                    break
                stack.pop()
            i += 1
        for c, ln in stack:
            fail(path, ln, 'unclosed "%s"' % c)


# 3. files -----------------------------------------------------------------------------------------------------------
BAD_EXT = ('.ff', '.iwi', '.exe', '.zone', '.ogg', '.wav', '.mp3', '.bik', '.bsp', '.xex', '.iso', '.xwm', '.flac')


def check_files(files):
    for path in files:
        if path.lower().endswith(BAD_EXT):
            fail(path, 1, 'game binary or asset: nothing from the game belongs in the repository')
        elif os.path.isfile(path) and os.path.getsize(path) > 20 * 1024 * 1024:
            fail(path, 1, 'file over 20 MB')


# 4. privacy ---------------------------------------------------------------------------------------------------------
USER_PATH = re.compile(r'[A-Za-z]:[\\/]+Users[\\/]+(?![<{%$]|user\b|User\b|name\b|you\b|YOU\b|Public\b|Default\b|\*)[^\\/\s"\'`)]+'
                       r'|/(?:home|Users)/(?![<{$]|user\b|runner\b|name\b)[A-Za-z][A-Za-z0-9_-]+/')


def check_privacy(files):
    for path in files:
        if path.lower().endswith(BAD_EXT + ('.otf', '.ttf', '.dll', '.png', '.jpg', '.zip')):
            continue
        data = open(path, 'rb').read()
        if b'\0' in data[:4096]:
            continue
        for n, line in enumerate(data.decode('utf-8', errors='replace').split('\n'), 1):
            m = USER_PATH.search(line)
            if m:
                fail(path, n, 'a personal path (%s): use <game dir> or %%USERPROFILE%% instead' % m.group(0))


# 5. changelog -------------------------------------------------------------------------------------------------------
def check_changelog(base):
    changed = [f for f in git('diff', '--name-only', base + '...HEAD').split('\n') if f]
    code = [f for f in changed if f.startswith(('src/', 'content/scripts/', 'content/patches/', 'content/release/'))]
    if not code:
        return
    missing = [c for c in ('CHANGELOG.md', 'CHANGELOG.ru.md') if c not in changed]
    if missing:
        warn('code or content changed (%d files) but %s not: the project keeps an English and a Russian entry '
             'under "Unreleased" for every change' % (len(code), ' and '.join(missing)))


# 5. generated headers --------------------------------------------------------------------------------------------
def check_generated():
    for tool in ('gen_menus.py', 'gen_settings_template.py', 'gen_natsites.py', 'check_raw_addresses.py'):  # (the last one has no --check: it checks by itself)
        r = subprocess.run([sys.executable, '-I', os.path.join('tools', tool)] + ([] if tool == 'check_raw_addresses.py' else ['--check']), capture_output=True, text=True)
        if r.returncode != 0:
            fail('tools/' + tool, 1, (r.stdout + r.stderr).strip() or 'the generated header is out of date')


def main():
    base = None
    if '--base' in sys.argv:
        base = sys.argv[sys.argv.index('--base') + 1]
    files = tracked()
    check_patches(files)
    check_gsc(files)
    check_files(files)
    check_privacy(files)
    check_generated()
    if base:
        check_changelog(base)
    print('%d tracked files checked: %d errors, %d warnings' % (len(files), len(errors), len(warnings)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
