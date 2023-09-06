// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QWidget>

#include "Core/AchievementManager.h"

class QGroupBox;
class QLabel;
class QProgressBar;
class QVBoxLayout;

class AchievementHeaderWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit AchievementHeaderWidget(QWidget* parent);
  void UpdateData();

private:
  QString GetPointsString(const QString& user_name,
                          const AchievementManager::PointSpread& point_spread) const;

  QGroupBox* m_common_box;
  QVBoxLayout* m_common_layout;

  QLabel* m_user_name;
  QLabel* m_user_points;
  QLabel* m_game_name;
  QLabel* m_game_points;
  QProgressBar* m_game_progress_hard;
  QProgressBar* m_game_progress_soft;
  QLabel* m_rich_presence;

  QGroupBox* m_user_box;
  QGroupBox* m_game_box;
};

#endif  // USE_RETRO_ACHIEVEMENTS
