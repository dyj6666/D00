from PyQt5.QtWidgets import (QTreeWidget, QTreeWidgetItem, QMenu,
                             QAbstractItemView, QInputDialog)
from PyQt5.QtCore import Qt
from model.variable import VariableInfo

class VariableTree(QTreeWidget):
    def __init__(self):
        super().__init__()
        self.setHeaderLabels(["变量名称", "ID", "类型", "当前值"])
        self.header().setStretchLastSection(True)
        self.setDragEnabled(True)
        self.setRootIsDecorated(False)
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)

        self.variables = {}          # vid -> QTreeWidgetItem
        self.alarm_thresholds = {}   # vid -> (low, high)
        self.subscribe_callback = None
        self.write_callback = None
        self.move_callback = None
        self.statistics_callback = None

        self.itemDoubleClicked.connect(self._on_double_click)
        self.setContextMenuPolicy(Qt.CustomContextMenu)
        self.customContextMenuRequested.connect(self._on_context_menu)

    def _on_context_menu(self, pos):
        item = self.currentItem()
        if not item:
            return
        vid = item.data(0, Qt.UserRole)
        if vid is None:
            return

        menu = QMenu(self)
        write_action = menu.addAction("写入新值")
        alarm_action = menu.addAction("设置报警阈值")
        stats_action = menu.addAction("数据统计")
        move_menu = menu.addMenu("移动到图表")

        plot_names = []
        if self.move_callback:
            plot_names = self.move_callback("get_plot_names", None)
            for pname in plot_names:
                move_menu.addAction(pname)

        action = menu.exec_(self.mapToGlobal(pos))
        if action == write_action and self.write_callback:
            self.write_callback(vid)
        elif action == alarm_action:
            self._set_alarm(vid)
        elif action == stats_action and self.statistics_callback:
            self.statistics_callback(vid)
        elif action and self.move_callback and action.text() in plot_names:
            self.move_callback("move", vid, action.text())

    def _set_alarm(self, vid):
        low, ok1 = QInputDialog.getDouble(self, "下限", f"变量 0x{vid:04X} 报警下限:")
        if not ok1:
            return
        high, ok2 = QInputDialog.getDouble(self, "上限", f"变量 0x{vid:04X} 报警上限:")
        if not ok2:
            return
        self.alarm_thresholds[vid] = (low, high)

    def _on_double_click(self, item, column):
        vid = item.data(0, Qt.UserRole)
        if self.subscribe_callback and vid is not None:
            self.subscribe_callback(vid)

    def update_variables(self, var_list: list[VariableInfo]):
        self.clear()
        self.variables.clear()
        for var in var_list:
            item = QTreeWidgetItem([var.name, f"0x{var.id:04X}", var.type_name, "--"])
            item.setData(0, Qt.UserRole, var.id)
            self.addTopLevelItem(item)
            self.variables[var.id] = item

    def update_value(self, vid: int, value):
        item = self.variables.get(vid)
        if item:
            item.setText(3, str(value))
            # 报警检查
            if vid in self.alarm_thresholds:
                low, high = self.alarm_thresholds[vid]
                if value < low or value > high:
                    item.setBackground(3, Qt.red)
                else:
                    item.setBackground(3, Qt.transparent)

    def get_selected_ids(self) -> list[int]:
        ids = []
        for item in self.selectedItems():
            vid = item.data(0, Qt.UserRole)
            if vid is not None:
                ids.append(vid)
        return ids

    def filter_variables(self, text):
        """根据搜索文本过滤变量树"""
        for i in range(self.topLevelItemCount()):
            item = self.topLevelItem(i)
            name = item.text(0)
            vid_str = item.text(1)
            match = text.lower() in name.lower() or text in vid_str
            item.setHidden(not match)