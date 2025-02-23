// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model;

import androidx.annotation.NonNull;

public class AbstractLegacySetting implements AbstractSetting
{
  protected final String mFile;
  protected final String mSection;
  protected final String mKey;

  public AbstractLegacySetting(String file, String section, String key)
  {
    mFile = file;
    mSection = section;
    mKey = key;
  }

  @Override
  public boolean isOverridden(@NonNull Settings settings)
  {
    return settings.isGameSpecific() && settings.getSection(mFile, mSection).exists(mKey);
  }

  @Override
  public boolean isRuntimeEditable()
  {
    return false;
  }

  @Override
  public boolean delete(@NonNull Settings settings)
  {
    return settings.getSection(mFile, mSection).delete(mKey);
  }
}
