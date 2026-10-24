#!/usr/bin/env python3
# Make the sfiii3 wide screen patch header from the sfiii3ws hack.
#
#   mkwidepatch.py <original simm1 dir> <sfiii3ws simm1 dir> <name> > header
#
# Both directories hold sfiii3-simm1.0 .. .3. The patch is the difference of
# the decrypted programs, minus the hack's changes that are not about the
# wide screen (region forced to Oceania and its warning text, used for the
# hack's credits).

import struct, sys

KEY1, KEY2 = 0xa55432b4, 0x0c129981	# sfiii3

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
		out += struct.pack('>I', w ^ mask(0x06000000 + i * 4))
	return out

SKIP = [(0x06005326, 0x06005328),	# region forced to 7
	(0x0613d900, 0x0613e000)]	# warning screen text

old, new, name = program(sys.argv[1]), program(sys.argv[2]), sys.argv[3]

blocks = []
for i in range(len(old)):
	a = 0x06000000 + i
	if old[i] == new[i] or any(s <= a < e for s, e in SKIP): continue
	if blocks and i - blocks[-1][1] <= 8: blocks[-1][1] = i
	else: blocks.append([i, i])

print('// sfiii3 wide screen patch, made by tools/cps3bench/mkwidepatch.py from the')
print('// sfiii3ws hack (20220908) and the program it is based on.')
print('// Addresses and bytes as the SH-2 sees them (decrypted, big endian).')
print()
print('static const UINT8 %s_bytes[] = {	// original, then patched' % name)
for s, e in blocks:
	print('\t' + ', '.join('0x%02x' % b for b in old[s:e + 1]) + ',')
	print('\t' + ', '.join('0x%02x' % b for b in new[s:e + 1]) + ',')
print('};')
print()
print('static const cps3_patch_block %s_blocks[] = {' % name)
for s, e in blocks:
	print('\t{ 0x%08x, %d },' % (0x06000000 + s, e - s + 1))
print('\t{ 0, 0 }')
print('};')
