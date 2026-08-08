# -*- coding: utf-8 -*-
"""深色工业风 QSS。"""

QSS = """
QMainWindow, QWidget { background: #1e2430; color: #d8dee9; font-size: 13px; }
QPlainTextEdit, QLineEdit, QTableWidget, QTreeWidget, QComboBox, QSpinBox {
    background: #141820; color: #d8dee9; border: 1px solid #2e3748;
    border-radius: 4px; selection-background-color: #2f6f9f;
}
QPlainTextEdit { font-family: Consolas, 'Courier New', monospace; }
QPushButton {
    background: #2a3346; color: #d8dee9; border: 1px solid #3a4a63;
    border-radius: 4px; padding: 4px 10px;
}
QPushButton:hover { background: #35435c; }
QPushButton:checked { background: #1f6f4f; border-color: #2f9f6f; }
QPushButton:disabled { color: #5b6577; background: #232a38; }
QHeaderView::section {
    background: #232a38; color: #9fb0c8; border: 1px solid #2e3748;
    padding: 3px;
}
QTableWidget { alternate-background-color: #1a212e; gridline-color: #2e3748; }
QTreeWidget::item { padding: 1px; }
QLabel { color: #aeb8c8; }
QTabWidget::pane { border: 1px solid #2e3748; }
QTabBar::tab {
    background: #232a38; color: #9fb0c8; padding: 5px 14px;
    border: 1px solid #2e3748; border-bottom: none;
    border-top-left-radius: 4px; border-top-right-radius: 4px;
}
QTabBar::tab:selected { background: #2f6f9f; color: #ffffff; }
QStatusBar { background: #232a38; color: #9fb0c8; }
QToolBar { background: #232a38; border-bottom: 1px solid #2e3748; spacing: 6px; }
QSplitter::handle { background: #2e3748; }
QScrollBar:vertical { background: #1a212e; width: 10px; }
QScrollBar::handle:vertical { background: #3a4a63; min-height: 24px; }
QScrollBar:horizontal { background: #1a212e; height: 10px; }
QScrollBar::handle:horizontal { background: #3a4a63; min-width: 24px; }
QCheckBox { color: #aeb8c8; spacing: 5px; }
QTableWidget::item:selected { background: #2f6f9f; color: #ffffff; }
"""
