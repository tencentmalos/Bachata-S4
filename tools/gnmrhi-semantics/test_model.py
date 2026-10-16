"""Scenario checks for the reconstruction, not tests against a running game."""

import unittest

from model import (Arena, LooseConstants, ResourceEntry, StageBindings,
                   commit_resource_tables, compute_dispatch_plan, decode_binding,
                   refresh_buffer_srv, walk_resource_map)


class SemanticScenarios(unittest.TestCase):
    def test_two_dispatches_keep_distinct_constant_and_table_snapshots(self):
        arena = Arena()
        constants = LooseConstants(bytearray(64))
        stage = StageBindings(0)
        constants.write(32, b"old!")
        old = constants.commit(arena, True)
        stage.bind(4, old.descriptor)
        constants.acknowledge_binding()
        old_table = stage.commit_compute(arena, 8, (2, 255, 255, 255, 255))
        constants.write(32, b"new!")
        new = constants.commit(arena, True)
        stage.bind(4, new.descriptor)
        new_table = stage.commit_compute(arena, 8, (2, 255, 255, 255, 255))
        self.assertEqual(old.copied[32:], b"old!")
        self.assertEqual(new.copied[32:], b"new!")
        self.assertNotEqual(old.address, new.address)
        self.assertEqual(old_table.words[4:], old.descriptor)
        self.assertEqual(new_table.words[4:], new.descriptor)
        self.assertNotEqual(old_table.pointer_packets, new_table.pointer_packets)

    def test_partial_update_retains_extent_only_when_requested(self):
        state = LooseConstants(bytearray(range(64)))
        state.write(33, b"X")
        first = state.commit(Arena(), True)
        self.assertEqual((len(first.copied), first.allocated_size), (34, 48))
        self.assertTrue(state.dirty)  # Upload itself does not acknowledge the bind.
        state.acknowledge_binding()
        state.write(0, b"Y")
        retained = state.commit(Arena(), False)
        self.assertEqual((len(retained.copied), retained.allocated_size), (48, 48))
        self.assertEqual(retained.copied[33], ord("X"))
        state.acknowledge_binding()
        state.write(0, b"Z")
        shrunk = state.commit(Arena(), True)
        self.assertEqual((len(shrunk.copied), shrunk.allocated_size), (1, 16))
        self.assertEqual(shrunk.descriptor, (0, 0x100001, 1, 0x20077FAC))

    def test_large_extent_switches_allocator_path(self):
        state = LooseConstants(bytearray(0x10020))
        state.write(0x10000, b"x")
        upload = state.commit(Arena(), True)
        self.assertEqual(upload.allocation_path, "transient_1KiB")
        self.assertEqual(upload.allocated_size, 0x10010)
        state.acknowledge_binding()
        self.assertIsNone(state.commit(Arena(), True))

    def test_resource_maps_join_ub_resources_to_shader_slots_and_clear_dirty(self):
        # Two prefix words; slot 1 starts at index 2. 0xffffffff terminates the run.
        maps = {"texture": [0, 2, 0x01000107, 0xFFFFFFFF],
                "srv": [0, 2, 0x01000003, 0xFFFFFFFF],
                "sampler": [0, 2, 0x01000205, 0xFFFFFFFF]}
        calls = []
        dirty = commit_resource_tables(0b1010, 0b0010, maps,
                                       {1: ["buffer", "texture", "sampler"]},
                                       lambda *args: calls.append(args))
        self.assertEqual(calls, [("texture", 7, "texture"), ("srv", 3, "buffer"),
                                 ("sampler", 5, "sampler")])
        self.assertEqual(dirty, 0)  # Includes bit 3, absent from shader's mask.
        self.assertEqual(ResourceEntry.decode(0x03012A07), ResourceEntry(3, 298, 7))

    def test_truncated_capture_is_rejected_instead_of_inventing_a_sentinel(self):
        with self.assertRaises(ValueError):
            list(walk_resource_map([1, 0x00000102], 0))
        with self.assertRaises(ValueError):
            list(walk_resource_map([100], 0))
        self.assertEqual(list(walk_resource_map([0], 0)), [])

    def test_direct_table_and_unused_bindings_have_different_effects(self):
        desc = (0x12345678, 0x100001, 4, 0x20077FAC)
        cs = StageBindings(0)
        cs.bind(0x8004, desc)
        self.assertEqual(cs.direct_packets[0],
                         (0xC0001000, 0x6875000D, 0xC0047602, 0x244, *desc))
        cs.bind(0xFFFF, desc)
        self.assertTrue(cs.dirty)
        empty = cs.commit_compute(Arena(), 4)
        self.assertEqual(empty.words, (None,) * 4)
        self.assertIsNone(cs.commit_compute(Arena(), 4))
        cs.bind(0x4004, desc)  # Bit 14 is masked in the observed binding helpers.
        self.assertEqual(cs.shadow[4:8], list(desc))
        self.assertEqual(decode_binding(0xFFFF).route, "unused")
        ps = StageBindings(1)
        ps.bind(0x8004, desc)
        self.assertEqual(ps.direct_packets[0][2:4], (0xC0047600, 0x10))

    def test_compute_table_pointers_use_dword_offsets(self):
        cs = StageBindings(0)
        cs.bind(4, (10, 11, 12, 13))
        table = cs.commit_compute(Arena(), 16, (2, 4, 255, 255, 255), (4, 8, 12, 14))
        self.assertEqual(table.pointer_packets,
                         ((0xC0027602, 0x242, 0, 1), (0xC0027602, 0x244, 16, 1)))
        self.assertEqual(table.words[:4], (None,) * 4)

    def test_srv_address_refresh_tracks_current_buffer_without_losing_format(self):
        old = (0x1000, 0xAB123005, 64, 0x20077FAC)
        new = refresh_buffer_srv(old, 0x123456789A)
        self.assertEqual(new, (0x3456789A, 0xAB123012, 64, 0x20077FAC))
        self.assertEqual(old[0], 0x1000)

    def test_auto_flush_only_gates_the_observed_dispatch_path(self):
        both = compute_dispatch_plan(True, True)
        self.assertIn("EOP", both[0])
        self.assertIn("EOS", both[-1])
        self.assertLess(both.index("LCUE_commit_compute"), both.index("sceGnmDispatchDirect"))
        self.assertEqual(compute_dispatch_plan(False, True)[-1], "sceGnmDispatchDirect")
        self.assertEqual(compute_dispatch_plan(True, False)[-1], "sceGnmDispatchDirect")

    def test_model_refuses_unreconstructed_or_out_of_domain_inputs(self):
        with self.assertRaises(ValueError):
            StageBindings(1).commit_compute(Arena(), 4)
        with self.assertRaises(ValueError):
            LooseConstants(bytearray(32)).write(31, b"xx")
        with self.assertRaises(ValueError):
            StageBindings(0).bind(1535, (1, 2, 3, 4))


if __name__ == "__main__":
    unittest.main()
