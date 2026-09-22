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

    def test_dolly_fov_cannot_bypass_roll(self):
        hook=CAMERA.split('float __fastcall cg_calc_fov_stub',1)[1].split('// Resolved inside',1)[0]
        self.assertLess(hook.index('apply_roll_to_view_axis();'),hook.index('fresh_dolly_fov()'))
        self.assertIn('landed <= target + 5',NATIVE)

    def test_actual_axis_expressions_preserve_orientation(self):
        block=CAMERA.split('axis[0] = cp * cy;',1)[1]
        block='axis[0] = cp * cy;'+block.split('const auto count',1)[0]
        stores=re.findall(r'axis\[(\d)\] (=|\*=) ([^;]+);',block)
        self.assertEqual(len(stores),12)
        for pitch in (-89,-20,0,55,89):
            for yaw in (-180,0,93):
                for roll in (-180,-20,0,20,180):
                    for handedness in (-1,1):
                        p,y,r=map(math.radians,(pitch,yaw,roll))
                        env=dict(sp=math.sin(p),cp=math.cos(p),sy=math.sin(y),cy=math.cos(y),sr=math.sin(r),cr=math.cos(r),lateral_sign=-handedness)
                        axis=[0.0]*9
                        for idx,op,expr in stores:
                            i=int(idx); value=eval(expr,{},env)
                            axis[i]=value if op=='=' else axis[i]*value
                        f,l,u=[axis[i:i+3] for i in (0,3,6)]
                        dot=lambda a,b:sum(x*y for x,y in zip(a,b))
                        for v in (f,l,u): self.assertAlmostEqual(dot(v,v),1)
                        self.assertAlmostEqual(dot(f,l),0)
                        self.assertAlmostEqual(dot(l,u),0)
                        cross=[l[1]*u[2]-l[2]*u[1],l[2]*u[0]-l[0]*u[2],l[0]*u[1]-l[1]*u[0]]
                        self.assertAlmostEqual(dot(f,cross),handedness)
                        self.assertAlmostEqual(f[2],-math.sin(p))

if __name__=='__main__': unittest.main()
