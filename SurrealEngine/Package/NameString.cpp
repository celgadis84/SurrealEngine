
#include "Precomp.h"
#include "NameString.h"

Array<std::string> NameString::Names;
std::unordered_map<std::string, int> NameString::CompareStringToIndex;
std::unordered_map<std::string, std::pair<int, int>> NameString::SpellStringToIndex;

#ifdef __N64__
// N64 port: the stock interner stores the upper-case compare string and the spelling in Names
// and a copy of each in two unordered_maps (~250 B per name in the 8 MB heap). Here: one string
// per spelling, CompareOf[spelled] = spelled index of the first case-insensitive spelling, and
// two open-addressing tables of spelled indices (-1 = free).
Array<int> NameString::CompareOf;
Array<int> NameString::SpellSlots;
Array<int> NameString::CmpSlots;

static inline unsigned NS_Upper(unsigned char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static unsigned NS_Hash(const std::string& s, bool ci)
{
	unsigned h = 2166136261u;
	for (unsigned char c : s)
	{
		h ^= ci ? NS_Upper(c) : c;
		h *= 16777619u;
	}
	return h;
}

static bool NS_EqualCI(const std::string& a, const std::string& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (NS_Upper((unsigned char)a[i]) != NS_Upper((unsigned char)b[i]))
			return false;
	return true;
}

// slot holding the match, or the free slot where it goes
static size_t NS_Find(const Array<int>& slots, const Array<std::string>& names, const std::string& s, bool ci)
{
	size_t mask = slots.size() - 1;
	size_t i = NS_Hash(s, ci) & mask;
	while (slots[i] != -1)
	{
		const std::string& n = names[slots[i]];
		if (ci ? NS_EqualCI(n, s) : n == s)
			return i;
		i = (i + 1) & mask;
	}
	return i;
}

static void NS_Grow(Array<int>& slots, const Array<std::string>& names, bool ci)
{
	Array<int> old = std::move(slots);
	slots = Array<int>();
	slots.resize(old.empty() ? 4096 : old.size() * 2, -1);
	size_t mask = slots.size() - 1;
	for (int idx : old)
	{
		if (idx == -1)
			continue;
		size_t i = NS_Hash(names[idx], ci) & mask;
		while (slots[i] != -1)
			i = (i + 1) & mask;
		slots[i] = idx;
	}
}

#ifdef UE_K64TABLES
// N64 port (UE_K64TABLES): every name of every package in the ROM is in the baked pool, so the
// package loader only stores u16 ids. Spellings become std::string only when ToString() asks
// (NS_Mat, kept for good: callers hold the reference). Names not in the pool (ini keys,
// string -> name casts, groups) go to the interner below with local indices; their global id
// is NS_DYN0 + local and their CompareIndex the pool class when one matches case-insensitively.
extern "C" {
#include <k64/romfs.h>
}
#include <cstring>
#include <cstdlib>

static const int NS_DYN0 = 0x10000;
int NameString::PoolN = 0;
const uint8_t* NameString::PoolSame = nullptr;
static const uint32_t* NS_PoolOffs = nullptr;
static const uint16_t* NS_PoolHash = nullptr;
static uint32_t NS_PoolMask = 0;
static const char* NS_PoolChars = nullptr;
static std::string** NS_Mat = nullptr;
static int NS_MatCount = 0;

bool NameString::LoadPoolN64(const char* path)
{
	if (PoolN)
		return false;
	k64_file_t f;
	if (!k64_romfs_open(&f, path) || f.packed || f.size < 16)
		return false;
	uint8_t* b = (uint8_t*)malloc((f.size + 15) & ~15u);
	if (!b || k64_romfs_read(&f, b, f.size) != f.size || memcmp(b, "K6NM", 4) != 0)
	{
		free(b);
		return false;
	}
	const uint32_t* h = (const uint32_t*)b;
	uint32_t n = h[1], hsize = h[2];
	uint32_t p = 16;
	NS_PoolOffs = (const uint32_t*)(b + p);
	p += 4 * n;
	PoolSame = b + p;
	p = (p + ((n + 7) >> 3) + 3) & ~3u;
	NS_PoolHash = (const uint16_t*)(b + p);
	p += 2 * hsize;
	NS_PoolChars = (const char*)(b + p);
	NS_PoolMask = hsize - 1;
	PoolN = (int)n;
	return true;
}

int NameString::MaterializedN64() { return NS_MatCount; }

const std::string& NameString::SpellN64(int idx)
{
	if (idx >= NS_DYN0)
		return Names[idx - NS_DYN0];
	if (idx >= PoolN) // no pool: only None (0) can be here
	{
		static const std::string none("None");
		return none;
	}
	if (!NS_Mat)
		NS_Mat = (std::string**)calloc(PoolN, sizeof(std::string*));
	std::string*& s = NS_Mat[idx];
	if (!s)
	{
		s = new std::string(NS_PoolChars + NS_PoolOffs[idx]);
		NS_MatCount++;
	}
	return *s;
}

static bool NS_EqualCIc(const char* a, const std::string& b)
{
	size_t i = 0;
	for (; i < b.size(); i++)
		if (!a[i] || NS_Upper((unsigned char)a[i]) != NS_Upper((unsigned char)b[i]))
			return false;
	return a[i] == 0;
}

// canonical pool id of value's case-insensitive class (-1: not in the pool); *spell = the pool
// id of this exact spelling, or -1
static int NS_PoolFind(const std::string& value, int* spell)
{
	*spell = -1;
	int n = NameString::PoolCountN64();
	if (!n)
		return -1;
	uint32_t i = NS_Hash(value, true) & NS_PoolMask;
	for (;;)
	{
		unsigned c = NS_PoolHash[i];
		if (c == 0xFFFF)
			return -1;
		if (NS_EqualCIc(NS_PoolChars + NS_PoolOffs[c], value))
		{
			for (int j = (int)c; j < n && (j == (int)c || NameString::PoolCmpN64(j) == (int)c); j++)
				if (strcmp(NS_PoolChars + NS_PoolOffs[j], value.c_str()) == 0)
				{
					*spell = j;
					break;
				}
			return (int)c;
		}
		i = (i + 1) & NS_PoolMask;
	}
}

void NameString::GetIndexN64(const std::string& value)
{
	if (value.empty())
	{
		CompareIndex = 0;
		SpelledIndex = 0;
		return;
	}

	int spell;
	int pc = NS_PoolFind(value, &spell);
	if (spell >= 0)
	{
		CompareIndex = pc;
		SpelledIndex = spell;
		return;
	}

	if (SpellSlots.empty())
	{
		NS_Grow(SpellSlots, Names, false);
		NS_Grow(CmpSlots, Names, true);
	}
	size_t s = NS_Find(SpellSlots, Names, value, false);
	if (SpellSlots[s] != -1)
	{
		CompareIndex = CompareOf[SpellSlots[s]];
		SpelledIndex = NS_DYN0 + SpellSlots[s];
		return;
	}

	size_t c = NS_Find(CmpSlots, Names, value, true);
	int local = (int)Names.size();
	int dcmp;
	if (pc >= 0)
		dcmp = pc;
	else if (CmpSlots[c] != -1)
		dcmp = CompareOf[CmpSlots[c]];
	else if (NS_EqualCIc("None", value))
		dcmp = 0;
	else
		dcmp = NS_DYN0 + local;
	Names.push_back(value);
	CompareOf.push_back(dcmp);
	SpellSlots[s] = local;
	if (pc < 0 && CmpSlots[c] == -1)
		CmpSlots[c] = local;
	if (Names.size() * 2 > SpellSlots.size())
	{
		NS_Grow(SpellSlots, Names, false);
		NS_Grow(CmpSlots, Names, true);
	}
	CompareIndex = dcmp;
	SpelledIndex = NS_DYN0 + local;
}
#else
void NameString::GetIndexN64(const std::string& value)
{
	if (Names.empty())
	{
		Names.push_back("None");
		CompareOf.push_back(0);
		NS_Grow(SpellSlots, Names, false);
		NS_Grow(CmpSlots, Names, true);
		SpellSlots[NS_Find(SpellSlots, Names, Names[0], false)] = 0;
		CmpSlots[NS_Find(CmpSlots, Names, Names[0], true)] = 0;
	}

	if (value.empty())
	{
		CompareIndex = 0;
		SpelledIndex = 0;
		return;
	}

	size_t s = NS_Find(SpellSlots, Names, value, false);
	if (SpellSlots[s] != -1)
	{
		SpelledIndex = SpellSlots[s];
		CompareIndex = CompareOf[SpelledIndex];
		return;
	}

	size_t c = NS_Find(CmpSlots, Names, value, true);
	int idx = (int)Names.size();
	int cmp = CmpSlots[c] != -1 ? CompareOf[CmpSlots[c]] : idx;
	Names.push_back(value);
	CompareOf.push_back(cmp);
	SpellSlots[s] = idx;
	if (CmpSlots[c] == -1)
		CmpSlots[c] = idx;
	if (Names.size() * 2 > SpellSlots.size())
	{
		NS_Grow(SpellSlots, Names, false);
		NS_Grow(CmpSlots, Names, true);
	}
	CompareIndex = cmp;
	SpelledIndex = idx;
}
#endif // UE_K64TABLES
#endif
