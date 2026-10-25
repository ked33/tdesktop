import sys, os

def resolve(arch):
    if sys.platform == 'darwin':
        os.environ['QT'] = '6.11.2'
    elif sys.platform == 'win32':
        # Ninja configure.bat does not receive x64. Native Tools still sets
        # Platform, which prepare.py already uses.
        if not arch:
            platform = os.environ.get('Platform', '').lower()
            if platform == 'x64':
                arch = 'x64'
            elif platform == 'x86':
                arch = 'x86'
            elif platform == 'arm64':
                arch = 'arm'
        if arch != 'arm' and ('qt5' in sys.argv
                or (arch != 'x64' and 'qt6' not in sys.argv)):
            print('Choosing Qt 5.')
            os.environ['QT'] = '5.15.19'
        else:
            print('Choosing Qt 6.')
            os.environ['QT'] = '6.11.2'
    return True
