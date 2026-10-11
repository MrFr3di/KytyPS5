"""KYTY-INDEX-001 G2-B2: actual source ordering tripwires + mutation controls.

Runs only on GitHub-hosted Windows; no Vulkan or emulator instance needed.
Structural source checks are deliberately not a semantic C++ concurrency proof.
"""
from __future__ import annotations

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEXTURE = "src/graphics/host_gpu/renderer/cache/textureCache.cpp"
SCHEDULER = "src/graphics/host_gpu/renderer/commandScheduler.cpp"
RENDER = "src/graphics/host_gpu/renderer/renderContext.cpp"


class SourceViolation(AssertionError):
    pass


def without_comments(source: str) -> str:
    # Targeted functions contain no URL-like strings. Strip comments so a
    # "fix" consisting of commenting out a safety operation never passes.
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", source, flags=re.S)


def method(source: str, name: str) -> str:
    clean = without_comments(source)
    matched = re.search(
        re.escape(name) + r"\s*\([^)]*\)\s*\{", clean
    )
    if matched is None:
        raise SourceViolation(f"Cannot locate method body: {name}")
    start = matched.end()
    depth = 1
    for index in range(start, len(clean)):
        if clean[index] == "{":
            depth += 1
        elif clean[index] == "}":
            depth -= 1
            if depth == 0:
                return clean[start:index]
    raise SourceViolation(f"Unbalanced method: {name}")


def order(body: str, *tokens: str) -> None:
    start = 0
    for token in tokens:
        found = body.find(token, start)
        if found < 0:
            raise SourceViolation(f"Missing or reordered required operation: {token}")
        start = found + len(token)


def verify_source_contracts(sources: dict[str, str]) -> None:
    deletion = method(sources[TEXTURE], "TextureCache::DeleteImage")
    order(
        deletion,
        "if (!image->depth_id)",
        "m_slot_images.ForEach(",
        "FreeImage(association);",
        "if (image->IsGpuModified())",
        "deleting a GPU-modified image without resolving its contents",
        "UnregisterImage(id);",
        "if (m_scheduler.Active())",
        "m_workload_telemetry.DeferredImageQueued();",
        "m_scheduler.DeferOperation([this, id] {",
        "m_slot_images.erase(id);",
        "m_workload_telemetry.DeferredImageCompleted();",
    )
    # No duplicate erasure inside the callback; immediate branch must erase.
    deferred = deletion.split("m_scheduler.DeferOperation([this, id] {", 1)[1]
    callback = deferred.split("});", 1)[0]
    if callback.count("m_slot_images.erase(id);") != 1:
        raise SourceViolation("Deferred callback must physically erase exactly once")
    immediate = deferred.split("});", 1)[1]
    order(
        immediate,
        "} else {",
        "m_slot_images.erase(id);",
        "ImageImmediateDestroyed",
    )
    unregister = method(sources[TEXTURE], "TextureCache::UnregisterImage")
    order(
        unregister,
        "m_lru_cache.Free(image.lru_id);",
        "m_total_used_memory -= accounted;",
        "image.registered = false;",
    )

    queued = method(sources[SCHEDULER], "CommandScheduler::QueueOperation")
    order(
        queued,
        "if (m_operation_state == OperationState::Open)",
        "queue.push({std::move(operation), CurrentTick()});",
        "lock.unlock();",
        "if (g_deferred_callback_scheduler != this)",
        "lock.unlock();",
        "operation();",
    )
    pop = method(sources[SCHEDULER], "CommandScheduler::PopPendingOperations")
    order(
        pop,
        "m_master.Refresh();",
        "!m_master.IsFree(m_pending_operations.front().tick)",
        "m_pending_operations.pop();",
        "WaitPriorityOperations(operation.tick);",
        "RunOperation(std::move(operation.callback));",
    )
    shutdown = method(sources[SCHEDULER], "CommandScheduler::Shutdown")
    order(
        shutdown,
        "m_operation_state = OperationState::Draining;",
        "m_master.Wait(CurrentTick() - 1);",
        "PopPendingOperations();",
        "DrainPriorityOperations();",
        "m_priority_thread.join();",
        "m_operation_state = OperationState::Closed;",
    )
    destruction = method(sources[RENDER], "RenderContext::~RenderContext")
    order(destruction, "ShutdownGpu();", "m_command_scheduler.Shutdown();")


def edit_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"Mutation fixture has {text.count(old)} occurrences: {old!r}")
    return text.replace(old, new, 1)


class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.original = {
            path: (ROOT / path).read_text(encoding="utf-8")
            for path in (TEXTURE, SCHEDULER, RENDER)
        }

    def mutated(self, path: str, old: str, new: str) -> None:
        bad = dict(self.original)
        bad[path] = edit_once(bad[path], old, new)
        with self.assertRaises(SourceViolation):
            verify_source_contracts(bad)

    def test_actual_source_contracts(self):
        verify_source_contracts(self.original)

    def test_whitespace_and_comment_are_not_safety_triggers(self):
        rewritten = dict(self.original)
        rewritten[TEXTURE] = edit_once(
            rewritten[TEXTURE],
            "UnregisterImage(id);\n\tif (m_scheduler.Active())",
            "UnregisterImage(id); /* harmless spacing */ \n\tif (m_scheduler.Active())",
        )
        verify_source_contracts(rewritten)

    def test_reject_missing_dirty_guard(self):
        self.mutated(
            TEXTURE,
            'EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\\n");',
            "/* unsafe omission */",
        )

    def test_reject_missing_deferred_physical_erase(self):
        self.mutated(
            TEXTURE,
            "m_scheduler.DeferOperation([this, id] {\n\t\t\tm_slot_images.erase(id);",
            "m_scheduler.DeferOperation([this, id] {\n\t\t\t/* missing erase */",
        )

    def test_reject_early_deferred_completion_count(self):
        self.mutated(
            TEXTURE,
            "m_slot_images.erase(id);\n#if defined(KYTY_TEXTURE_WORKLOAD_TELEMETRY) && KYTY_TEXTURE_WORKLOAD_TELEMETRY\n\t\t\tif (m_workload_telemetry_enabled) m_workload_telemetry.DeferredImageCompleted();",
            "m_workload_telemetry.DeferredImageCompleted();\n\t\t\tm_slot_images.erase(id);",
        )

    def test_reject_missing_unregistration(self):
        self.mutated(
            TEXTURE,
            "UnregisterImage(id);\n\tif (m_scheduler.Active())",
            "/* UnregisterImage missing */\n\tif (m_scheduler.Active())",
        )

    def test_reject_deferred_queue_without_gpu_tick(self):
        self.mutated(
            SCHEDULER,
            "queue.push({std::move(operation), CurrentTick()});",
            "queue.push({std::move(operation), 0});",
        )

    def test_reject_callback_without_completed_tick(self):
        self.mutated(
            SCHEDULER,
            "!m_master.IsFree(m_pending_operations.front().tick)",
            "false /* unsafe completed-tick assumption */",
        )

    def test_reject_priority_wait_elision(self):
        self.mutated(
            SCHEDULER,
            "WaitPriorityOperations(operation.tick);",
            "/* priority wait removed */",
        )

    def test_reject_callback_before_lock_unlock(self):
        self.mutated(
            SCHEDULER,
            "\tlock.unlock();\n\toperation();",
            "\toperation();\n\tlock.unlock();",
        )

    def test_reject_shutdown_without_priority_drain(self):
        self.mutated(
            SCHEDULER,
            "\tDrainPriorityOperations();\n\tm_priority_thread.request_stop();",
            "\t/* priority drain removed */\n\tm_priority_thread.request_stop();",
        )

    def test_reject_teardown_without_scheduler_shutdown(self):
        self.mutated(
            RENDER,
            "ShutdownGpu();\n\tm_command_scheduler.Shutdown();",
            "ShutdownGpu();\n\t/* missing scheduler shutdown */",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
