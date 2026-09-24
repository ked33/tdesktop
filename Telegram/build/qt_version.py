import sys, os

def resolve(arch):
    if sys.platform == 'darwin':
        os.environ['QT'] = '6.11.2'
    elif sys.platform == 'win32':
        # x64 on this branch uses Qt 6.11.2. ANGLE exists only in the Qt 5
        # build; Qt 6 draws the window with QRhi on Direct3D 11.
        if arch == 'arm' or arch == 'x64' or 'qt6' in sys.argv:
            print('Choosing Qt 6.')
            os.environ['QT'] = '6.11.2'
        else:
            print('Choosing Qt 5.')
            os.environ['QT'] = '5.15.19'
    return True
