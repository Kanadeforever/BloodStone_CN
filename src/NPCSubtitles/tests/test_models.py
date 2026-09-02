import struct,unittest

def valid_bli3(blob:bytes)->bool:
    return len(blob)>=32 and blob[:8]==b'BSCNLI3\0' and struct.unpack_from('<I',blob,8)[0]==3 and struct.unpack_from('<I',blob,12)[0]==32

def score_text(t:str)->int:
    if len(t)<6 or len(t)>420:return -1
    letters=sum(c.isascii() and c.isalpha() for c in t)
    spaces=t.count(' ')
    words=sum(1 for i,c in enumerate(t) if c.isascii() and c.isalpha() and (i==0 or not t[i-1].isalpha()))
    if letters<5 or words<2 or spaces<1:return -1
    if any(x in t for x in ('\\','/','_','.loc','.wav','%s')):return -1
    lower=sum(c.islower() for c in t)
    punct=sum(c in ".,!?'-:" for c in t)
    return letters*2+spaces*3+lower+punct*2+min(len(t),120)

class Models(unittest.TestCase):
    def test_bli3(self):
        good=struct.pack('<8sIIIIII',b'BSCNLI3\0',3,32,64,7665,221,0)
        self.assertTrue(valid_bli3(good))
        self.assertFalse(valid_bli3(good.replace(b'BSCNLI3',b'BSCNLI2')))
    def test_english_dialogue_score(self):
        self.assertGreater(score_text('What are you doing here? Get back to work.'),0)
        self.assertEqual(score_text('Monaco_01_CW01_199'),-1)
        self.assertEqual(score_text('C:\\game\\foo.loc'),-1)
    def test_short_noise_rejected(self):
        self.assertEqual(score_text('AAA'),-1)
        self.assertEqual(score_text('MP7'),-1)
if __name__=='__main__':unittest.main()
