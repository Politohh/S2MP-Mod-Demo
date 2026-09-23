"""Offline regression checks. These do not establish in-game behaviour."""
from pathlib import Path
import math, re, unittest
ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT/'src/demo/demo_native.cpp').read_text(encoding='utf-8-sig')
CAMERA = (ROOT/'src/demo/demo_camera.cpp').read_text(encoding='utf-8-sig')

class RegressionTests(unittest.TestCase):
    def test_reported_clock_mismatch(self):
        # Execute the actual alignment stores against the numbers from build 13.
        block = NATIVE.split('const int smooth_before = field(6368);',1)[1].split('Console::printf',1)[0]
        for snapshot, requested in [(323350,309200),(313850,309200),(313800,309200)]:
            fields = {6345:snapshot,6365:requested,6368:requested,6369:requested,6370:0}
            for index, expression in re.findall(r'field\((\d+)\) = ([^;]+);',block):
                fields[int(index)] = eval(expression,{},dict(s_jumped=snapshot))
            self.assertEqual(fields[6345], snapshot)
            self.assertEqual(fields[6368], snapshot)
            self.assertEqual(fields[6365], snapshot)
            self.assertEqual(fields[6369], snapshot)
            self.assertEqual(fields[6370], 0)
            self.assertIn('*realtime = s_jumped;', block)

    def test_failed_axis_experiment_removed(self):
        self.assertNotIn('apply_roll_to_view_axis', CAMERA)
        self.assertNotIn('axis[0] =', CAMERA)

    def test_failed_seek_does_not_start_playback(self):
        player=(ROOT/'src/demo/demo_player.cpp').read_text(encoding='utf-8-sig')
        command=player.split('void cmd_seek_to()',1)[1].split('void cmd_speed()',1)[0]
        gate=command.index('if (!seek_absolute(')
        self.assertLess(gate, command.index('toggle_pause()'))
        self.assertIn('return;',command[gate:command.index('toggle_pause()')])
        self.assertNotIn('target = pick_time;',NATIVE)

if __name__=='__main__': unittest.main()
