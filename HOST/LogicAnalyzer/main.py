"""LogicAnalyzer Pro 入口"""
import sys

from pyqtgraph.Qt import QtWidgets

from ui.main_window import MainWindow


def main():
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("LogicAnalyzer Pro")
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
