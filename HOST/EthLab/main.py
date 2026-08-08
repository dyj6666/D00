#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""EthLab - D00 以太网分析控制台入口。"""

import sys

from PyQt5.QtWidgets import QApplication

from ui.main_window import MainWindow


def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
