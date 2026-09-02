import unittest

def normalize_body(text:str)->str:
    text=text.strip()
    pos=-1
    for i,c in enumerate(text[:32]):
        if c in ':：' and 0<i<=24:
            pos=i;break
        if c in '\r\n':break
    if pos>=0 and text[pos+1:].strip(): text=text[pos+1:].strip()
    return ' '.join(text.lower().split())

class Fingerprint(unittest.TestCase):
    def test_story_speaker_body(self):
        self.assertEqual(normalize_body('Bond: 不要动。'), normalize_body('不要动。'))
    def test_unrelated_text_differs(self):
        self.assertNotEqual(normalize_body('任务目标：前往赌场'), normalize_body('离开这里'))

if __name__=='__main__':unittest.main()
