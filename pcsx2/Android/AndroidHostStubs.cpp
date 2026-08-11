// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Frontend-host stubs for the Android emucore shared library, whose JNI
// frontend defines neither of these. They live in their OWN translation unit —
// separate from AndroidStubs.cpp's platform stubs — so that a real frontend
// linking libpcsx2.a on Android (pcsx2-gsrunner) never pulls this archive
// member: the frontend's own definitions satisfy the references first, and
// archive semantics leave this object out of the link entirely.

#include "PrecompiledHeader.h"

#include "Input/InputManager.h"

// g_host_hotkeys - normally defined in pcsx2-qt
BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

// Host::SetMouseLock - no mouse lock on Android
void Host::SetMouseLock(bool state)
{
}
