#!/usr/bin/env python3
# Make the sfiii3 wide screen patch header from the sfiii3ws hack.
#
#   mkwidepatch.py <990512 simm1 dir> <sfiii3ws simm1 dir> [<990608 simm1 dir>] > header
#
# Each directory holds sfiii3-simm1.0 .. .3. The patch is the difference of
# the decrypted programs of the hack and of the 990512 program it is based on,
# minus the hack's changes that are not about the wide screen (region forced
# to Oceania and its warning text, used for the hack's credits).
#
# With the 990608 program the patch is also ported to it: every block is found
# by its surrounding bytes, the ROM addresses in it are relocated, and the new
# code and tables that the hack puts in free space go to free space there.

import struct, sys

KEY1, KEY2 = 0xa55432b4, 0x0c129981	# sfiii3
BASE = 0x06000000

def rol(v, n): return ((v << n) | (v >> (16 - n))) & 0xffff

def rotxor(val, x):
	res = (val + rol(val, 2)) & 0xffff
	return (rol(res, 4) ^ (res & (val ^ x))) & 0xffff

def mask(addr):
	addr ^= KEY1
	val = (addr & 0xffff) ^ 0xffff
	val = rotxor(val, KEY2 & 0xffff)
	val ^= ((addr >> 16) ^ 0xffff) & 0xffff
	val = rotxor(val, KEY2 >> 16)
	val ^= (addr & 0xffff) ^ (KEY2 & 0xffff)
	return val | (val << 16)

def program(d):
	s = [open('%s/sfiii3-simm1.%d' % (d, i), 'rb').read() for i in range(4)]
	out = bytearray()
	for i in range(len(s[0])):
		w = (s[0][i] << 24) | (s[1][i] << 16) | (s[2][i] << 8) | s[3][i]
		out += struct.pack('>I', w ^ mask(BASE + i * 4))
	return out

SKIP = [(0x06005326, 0x06005328),	# region forced to 7
	(0x0613d900, 0x0613e000)]	# warning screen text

def word(b, o): return struct.unpack('>I', b[o:o + 4])[0]
def is_rom(v): return BASE <= v < BASE + 0x1000000

def diff_blocks(old, new):
	"""blocks [start, end) of differences, 4-byte aligned"""
	blocks = []
	for i in range(len(old)):
		if old[i] == new[i] or any(s <= BASE + i < e for s, e in SKIP): continue
		s, e = i & ~3, (i + 4) & ~3
		if blocks and s - blocks[-1][1] <= 8: blocks[-1][1] = e
		else: blocks.append([s, e])
	return blocks

class Port:
	"""where things of the 990512 program are in the 990608 one"""
	def __init__(self, r1, n6, new_areas):
		self.r1, self.n6 = r1, n6
		self.new_areas = new_areas		# [(r1 offset, length, n6 offset)]
		self.located = []				# same, for the patch blocks

	def unique(self, pat):
		i = self.n6.find(pat)
		if i < 0 or self.n6.find(pat, i + 1) >= 0: return None
		return i

	def locate(self, o, length=0):
		"""offset in 990608 of the bytes at offset o (length l) of 990512"""
		for before, after in ((32, 32), (24, 24), (16, 16), (0, 32), (32, 0), (12, 12), (0, 16), (16, 0), (8, 8)):
			s = max(0, o - before); e = o + length + after
			i = self.unique(bytes(self.r1[s:e]))
			if i is not None: return i + (o - s)
		return None

	def reloc(self, v):
		# in the free space or in another patched block: move with it
		for r, l, n in self.new_areas + self.located:
			if BASE + r <= v < BASE + r + l: return BASE + n + (v - BASE - r)
		o = self.locate(v - BASE)
		if o is None: raise SystemExit('cannot relocate %08x' % v)
		return BASE + o

def port(r1, hack, n6, blocks):
	# the hack's new code and tables, written over zeros that can't be found by
	# the bytes around, go to the free space at the same place in the 990608
	# program, or where the data around moved
	new_areas = []
	p = Port(r1, n6, new_areas)
	for s, e in blocks:
		if any(r1[s:e]) or p.locate(s, e - s) is not None: continue
		for cand in (s, s + 0x14c):
			if not any(n6[cand - 8:cand + (e - s) + 8]):
				new_areas.append((s, e - s, cand)); break
		else:
			raise SystemExit('no free space for %08x' % (BASE + s))

	# where each block goes
	targets = []
	for s, e in blocks:
		area = [n for r, l, n in new_areas if r == s]
		if area: t = area[0]
		else:
			t = p.locate(s, e - s)
			if t is None:
				# take the offset of a block near that was located
				# (the bytes may already be the patched ones: Capcom made some of
				# the same changes in 990608)
				def same(cand, k):	# word k (from s) of 990512 at cand of 990608
					a, b = word(r1, s + k), word(n6, cand + k)
					if is_rom(a):
						try: return b == p.reloc(a)
						except SystemExit: return False
					return all(n6[cand + k + i] in (r1[s + k + i], hack[s + k + i]) for i in range(4))
				for d in sorted({x[2] - x[0] for x in p.located}, key=lambda d: abs(d)):
					if all(same(s + d, k) for k in range(-8, e - s, 4)):
						t = s + d; break
			if t is None:
				# pointer tables: look for the relocated original words
				pat = b''.join(struct.pack('>I', p.reloc(word(r1, k)) if is_rom(word(r1, k)) else word(r1, k))
						for k in range(s, e, 4))
				t = p.unique(pat)
			if t is None: raise SystemExit('cannot locate block %08x' % (BASE + s))
		targets.append(t)
		p.located.append((s, e - s, t))

	out = []
	for (s, e), t in zip(blocks, targets):
		new = bytearray(hack[s:e])
		for k in range(0, e - s, 4):
			v, o = word(hack, s + k), word(r1, s + k)
			if is_rom(v) and (v != o or word(n6, t + k) != o):
				new[k:k + 4] = struct.pack('>I', p.reloc(v))
		if bytes(n6[t:t + e - s]) == bytes(new): continue	# already so
		out.append((s, e, t, bytes(n6[t:t + e - s]), bytes(new)))
	return out

def emit(name, items):
	print('static const UINT8 %s_bytes[] = {	// original, then patched' % name)
	for addr, old, new in items:
		print('\t' + ', '.join('0x%02x' % b for b in old) + ',')
		print('\t' + ', '.join('0x%02x' % b for b in new) + ',')
	print('};')
	print()
	print('static const cps3_patch_block %s_blocks[] = {' % name)
	for addr, old, new in items:
		print('\t{ 0x%08x, %d },' % (addr, len(old)))
	print('\t{ 0, 0 }')
	print('};')
	print()

r1, hack = program(sys.argv[1]), program(sys.argv[2])
blocks = diff_blocks(r1, hack)

print('// sfiii3 wide screen patch, made by tools/cps3bench/mkwidepatch.py from the')
print('// sfiii3ws hack (20220908) and the 990512 program it is based on, and ported')
print('// to the 990608 program. Addresses and bytes as the SH-2 sees them')
print('// (decrypted, big endian).')
print()
emit('sfiii3_wide_990512', [(BASE + s, bytes(r1[s:e]), bytes(hack[s:e])) for s, e in blocks])
if len(sys.argv) > 3:
	n6 = program(sys.argv[3])
	emit('sfiii3_wide_990608', [(BASE + t, old, new) for s, e, t, old, new in port(r1, hack, n6, blocks)])
