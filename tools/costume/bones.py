"""Which bone of the imported model stands in for which bone of the game's.

The two skeletons turned out to be near enough the same size and facing the
same way, so this is a name map rather than a conversion. The game's sleeve
bones (sode) have no counterpart and ride the arm they hang from; the eyelid
is head-only and never reached by body geometry."""

MAP = {
    'bb_hip':      'pelvis',
    'bb_hara':     'spine lower',
    'bb_mune':     'spine upper',
    'bb_kubi':     'head neck lower',
    'bb_head':     'head neck upper',

    'bb_l_kata':   'arm left shoulder 1',
    'bb_l_ude1':   'arm left shoulder 2',
    'bb_l_ude2':   'arm left elbow',
    'bb_l_sode1':  'arm left shoulder 2',
    'bb_l_sode2':  'arm left elbow',
    'bb_l_hand1':  'arm left wrist',
    'bb_l_hand2':  'arm left wrist',

    'bb_r_kata':   'arm right shoulder 1',
    'bb_r_ude1':   'arm right shoulder 2',
    'bb_r_ude2':   'arm right elbow',
    'bb_r_sode1':  'arm right shoulder 2',
    'bb_r_sode2':  'arm right elbow',
    'bb_r_hand1':  'arm right wrist',
    'bb_r_hand2':  'arm right wrist',

    'bb_l_asi1':   'leg left thigh',
    'bb_l_asi2':   'leg left knee',
    'bb_l_asi3':   'leg left ankle',
    'bb_r_asi1':   'leg right thigh',
    'bb_r_asi2':   'leg right knee',
    'bb_r_asi3':   'leg right ankle',

    # The game's three skirt bones all sit behind the hips: they carry the
    # long back flap of Kurenai's kimono, so they take the model's back
    # skirt bones, not the front ones.
    'bb_c_skirt':  'skirt back middle 1a',
    'bb_l_skirt':  'skirt back left 1a',
    'bb_r_skirt':  'skirt back right 1a',
}

# An imported bone with no game bone of its own is carried by this one.
FALLBACK = [
    ('arm left finger',   'bb_l_hand2'),
    ('arm right finger',  'bb_r_hand2'),
    ('arm left hand',     'bb_l_hand1'),
    ('arm right hand',    'bb_r_hand1'),
    ('leg left toe',      'bb_l_asi3'),
    ('leg right toe',     'bb_r_asi3'),
    ('skirt back left',   'bb_l_skirt'),
    ('skirt back right',  'bb_r_skirt'),
    ('skirt back middle', 'bb_c_skirt'),
    ('skirt left',        'bb_l_skirt'),
    ('skirt right',       'bb_r_skirt'),
    ('skirt middle',      'bb_c_skirt'),
    ('breast',            'bb_mune'),
    ('head',              'bb_head'),
    ('neck',              'bb_kubi'),
    ('spine',             'bb_mune'),
    ('pelvis',            'bb_hip'),
    ('root',              'bb_hip'),
]


def game_bone_for(xps_name):
    for g, x in MAP.items():
        if x == xps_name:
            return g
    low = xps_name.lower()
    for prefix, g in FALLBACK:
        if low.startswith(prefix):
            return g
    return None
