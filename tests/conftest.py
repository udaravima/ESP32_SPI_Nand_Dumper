import os, sys
_here = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(_here, "..", "tools"))   # chipdb, gen_profiles
sys.path.insert(0, os.path.join(_here, ".."))            # project root (existing tests' convention)
