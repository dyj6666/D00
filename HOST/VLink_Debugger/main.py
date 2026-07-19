import sys
from PyQt5.QtWidgets import QApplication
from PyQt5.QtCore import Qt
from ui.main_window import MainWindow

if __name__ == "__main__":
    app = QApplication(sys.argv)
    # 深色主题
    app.setStyle("Fusion")
    dark_palette = app.palette()
    dark_palette.setColor(dark_palette.Window, Qt.black)
    dark_palette.setColor(dark_palette.WindowText, Qt.white)
    dark_palette.setColor(dark_palette.Base, Qt.darkGray)
    dark_palette.setColor(dark_palette.AlternateBase, Qt.gray)
    dark_palette.setColor(dark_palette.ToolTipBase, Qt.white)
    dark_palette.setColor(dark_palette.ToolTipText, Qt.white)
    dark_palette.setColor(dark_palette.Text, Qt.white)
    dark_palette.setColor(dark_palette.Button, Qt.darkGray)
    dark_palette.setColor(dark_palette.ButtonText, Qt.white)
    dark_palette.setColor(dark_palette.BrightText, Qt.red)
    dark_palette.setColor(dark_palette.Link, Qt.cyan)
    dark_palette.setColor(dark_palette.Highlight, Qt.lightGray)
    dark_palette.setColor(dark_palette.HighlightedText, Qt.black)
    app.setPalette(dark_palette)

    window = MainWindow()
    window.show()
    sys.exit(app.exec_())