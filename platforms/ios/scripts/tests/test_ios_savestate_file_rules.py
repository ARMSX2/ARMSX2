"""A slow or stale save-state task can't write into a dead frame, another game, or an empty slot."""

import unittest

from ios_source import CPP, block, read, without_comments


HOST = CPP / "IOS/HostImpls.mm"
MAIN = CPP / "ios_main.mm"
SCENE = CPP / "IOS/SceneDelegate.mm"
BRIDGE = CPP / "ARMSX2Bridge.mm"


class SaveStateFileRules(unittest.TestCase):
    """RunOnCPUThread gave up after 1 s and left the task queued. The save then ran anyway and
    wrote its result through the caller's dead stack frame, or ran in the next game's session."""

    def test_a_timed_out_task_is_dropped_or_waited_for(self):
        run = block(without_comments(read(HOST)), "void RunOnCPUThread(")
        self.assertIn("if (!task->started)", run)
        self.assertIn("task->cancelled = true;", run)
        self.assertIn("task->cv.wait(lock", run)

    def test_the_queue_skips_cancelled_tasks(self):
        drain = block(without_comments(read(MAIN)), "void ARMSX2DrainCPUThreadTasks()")
        self.assertIn("!task->cancelled", drain)

    def test_tasks_do_not_survive_the_vm(self):
        scene = without_comments(read(SCENE))
        after_stop = scene[scene.find("VMManager::Shutdown(false);"):]
        self.assertIn("ARMSX2DiscardCPUThreadTasks();", after_stop)

    def test_saves_check_the_game_and_keep_the_old_state(self):
        bridge = without_comments(read(BRIDGE))
        save = block(bridge, "static void ARMSX2WriteSaveState(")
        self.assertIn("ARMSX2SaveStateIdentityMatches(serial, crc)", save)
        self.assertIn("if (!result && existed)", save)
        restore = block(bridge, "static void ARMSX2RestoreSaveStateBackup(")
        self.assertIn("RENAME_EXCL", restore)

    def test_auto_save_writes_only_its_own_slot(self):
        """Nobody asked for an auto-save, so it must never land in a slot the player saved to, and
        it checks the memory cards on the CPU thread, at the moment it saves."""
        bridge = without_comments(read(BRIDGE))
        auto = block(bridge, "+ (void)autoSaveLeavingGame:")
        self.assertIn("ARMSX2WriteSaveState(VMManager::SAVESTATE_SLOT_AUTOSAVE, true,", auto)
        save = block(bridge, "static void ARMSX2WriteSaveState(")
        cpu = save[save.find("Host::RunOnCPUThread("):]
        self.assertIn("FileMcd_IsAutoEjecting()", cpu)
        self.assertIn("MemcardBusy::IsBusy()", cpu)


if __name__ == "__main__":
    unittest.main()
