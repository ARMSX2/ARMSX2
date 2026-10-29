// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The names `-uishot dialog:<name>` resolves (see QtShot.h). A dialog that is only reached through a button
// on a settings page does not need an entry: use  settings:<page>|click=<buttonObjectName>  instead.
// Add a line for a dialog that takes no button to open, or that needs constructor arguments. A dialog can
// equally put its QTSHOT_DIALOG line at the bottom of its own .cpp and leave this file alone.

#include "QtShot.h"

#include "AboutDialog.h"
#include "CoverDownloadDialog.h"
#include "SetupWizardDialog.h"

QTSHOT_DIALOG("about", "About ARMSX2", [](const QtShot::Context&) { return new AboutDialog(nullptr); });
QTSHOT_DIALOG("cover-downloader", "Tools > Download Covers", [](const QtShot::Context&) { return new CoverDownloadDialog(nullptr); });
QTSHOT_DIALOG("setup-wizard", "First-run setup wizard", [](const QtShot::Context&) { return new SetupWizardDialog(); });
