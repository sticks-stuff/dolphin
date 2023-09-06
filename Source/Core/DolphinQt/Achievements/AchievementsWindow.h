// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QDialog>

#include "Core/AchievementManager.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"

class AchievementHeaderWidget;
class AchievementProgressWidget;
class QDialogButtonBox;
class QTabWidget;
class UpdateCallback;

class AchievementsWindow : public QDialog
{
  Q_OBJECT
public:
  explicit AchievementsWindow(QWidget* parent);
  void UpdateData();

private:
  void CreateMainLayout();
  void showEvent(QShowEvent* event);
  void ConnectWidgets();

  AchievementHeaderWidget* m_header_widget;
  QTabWidget* m_tab_widget;
  AchievementProgressWidget* m_progress_widget;
  QDialogButtonBox* m_button_box;
};

#endif  // USE_RETRO_ACHIEVEMENTS
