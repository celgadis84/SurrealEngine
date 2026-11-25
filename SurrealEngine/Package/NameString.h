#pragma once

#include <vector>
#include <unordered_map>

class NameString
{
public:
	NameString() { }
	NameString(const char* str) { GetIndex(str); }
	NameString(const std::string& str) { GetIndex(str); }
	NameString(const NameString& other) = default;
	NameString& operator=(const NameString&) = default;

	bool IsNone() const { return CompareIndex == 0; }

#ifdef UE_K64TABLES
	const std::string& ToString() const { return SpellN64(SpelledIndex); }
#else
	const std::string& ToString() const { return Names[SpelledIndex]; }
#endif

	bool operator==(const char* other) const { return *this == NameString(other); }
	bool operator==(const std::string& other) const { return *this == NameString(other); }
	bool operator!=(const char* other) const { return *this != NameString(other); }
	bool operator!=(const std::string& other) const { return *this != NameString(other); }

	bool operator==(const NameString& other) const { return CompareIndex == other.CompareIndex; }
	bool operator!=(const NameString& other) const { return CompareIndex != other.CompareIndex; }
	bool operator<(const NameString& other) const { return CompareIndex < other.CompareIndex; }
	bool operator>(const NameString& other) const { return CompareIndex > other.CompareIndex; }
	bool operator<=(const NameString& other) const { return CompareIndex <= other.CompareIndex; }
	bool operator>=(const NameString& other) const { return CompareIndex >= other.CompareIndex; }

	int GetCompareIndex() const { return CompareIndex; }

#ifdef UE_K64TABLES
	// N64 port: baked name pool (baker/unrtab.py -> System/names.k64nm), loaded by main() before
	// the static constructors. Ids 0..PoolN-1 = pool spellings (0 = None), CompareIndex = first id
	// of the case-insensitive run; names first seen at run time get ids >= 0x10000 (Names).
	static bool LoadPoolN64(const char* path);
	static int PoolCountN64() { return PoolN; }
	static int PoolCmpN64(int id) { while (PoolSame[id >> 3] & (1 << (id & 7))) id--; return id; }
	static NameString FromPoolN64(int id) { NameString n; n.CompareIndex = PoolCmpN64(id); n.SpelledIndex = id; return n; }
	static int MaterializedN64();
	static int DynamicCountN64() { return (int)Names.size(); }
#endif

private:
	int CompareIndex = 0;
	int SpelledIndex = 0;

	void GetIndex(const std::string& value)
	{
#ifdef __N64__
		GetIndexN64(value);
		return;
#endif
		// Have we seen this spelling before?
		auto it = SpellStringToIndex.find(value);
		if (it != SpellStringToIndex.end())
		{
			CompareIndex = it->second.first;
			SpelledIndex = it->second.second;
			return;
		}

		// Initialize list with None as index 0
		if (Names.empty())
		{
			Names.push_back("None");
			CompareStringToIndex["NONE"] = 0;
			SpellStringToIndex["None"] = { 0, 0 };
		}

		// Any empty name string means None
		if (value.empty())
		{
			CompareIndex = 0;
			SpelledIndex = 0;
			return;
		}

		// Create case insensitive spelling string
		static const int stricmptable[] =
		{
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
			0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
			0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
			0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
			0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
			0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
			0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
			0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
			0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
			0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
			0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
			0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
			0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
			0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
			0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
			0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
		};

		std::string compareValue = value;
		for (size_t i = 0, count = compareValue.size(); i < count; i++)
		{
			compareValue[i] = (char)stricmptable[(uint8_t)compareValue[i]];
		}

		// Do we have a compare index?
		auto it2 = CompareStringToIndex.find(compareValue);
		if (it2 != CompareStringToIndex.end())
		{
			CompareIndex = it2->second;
		}
		else
		{
			CompareIndex = (int)Names.size();
			Names.push_back(compareValue);
			CompareStringToIndex[compareValue] = CompareIndex;
		}

		// Create spellstring index
		SpelledIndex = (int)Names.size();
		Names.push_back(value);
		SpellStringToIndex[value] = { CompareIndex, SpelledIndex };
	}

	static Array<std::string> Names;
	static std::unordered_map<std::string, int> CompareStringToIndex;
	static std::unordered_map<std::string, std::pair<int, int>> SpellStringToIndex;
#ifdef __N64__
	// N64 port: compact interner (NameString.cpp). One string per spelling, CompareIndex = the
	// spelled index of the first spelling; open-addressing tables of indices, no string copies.
	void GetIndexN64(const std::string& value);
	static Array<int> CompareOf;
	static Array<int> SpellSlots;
	static Array<int> CmpSlots;
#endif
#ifdef UE_K64TABLES
	static const std::string& SpellN64(int idx);
	static int PoolN;
	static const uint8_t* PoolSame;
#endif
};

#ifdef __N64__
// N64 port: per-package NameString -> index map (find / end / operator[] of the std::map it
// replaces), open addressing on CompareIndex: ~21 B per name instead of a 48 B tree node.
class NameIndexMap
{
public:
	struct Slot { NameString first; int second = 0; bool used = false; };

	Slot* find(const NameString& key)
	{
		if (slots.empty()) return nullptr;
		size_t i = Probe(key);
		return slots[i].used ? &slots[i] : nullptr;
	}
	Slot* end() { return nullptr; }

	int& operator[](const NameString& key)
	{
		if ((count + 1) * 4 > slots.size() * 3) Grow();
		size_t i = Probe(key);
		if (!slots[i].used)
		{
			slots[i].first = key;
			slots[i].second = 0;
			slots[i].used = true;
			count++;
		}
		return slots[i].second;
	}

	void reserve(size_t n)
	{
		while (n * 4 > slots.size() * 3) Grow();
	}

private:
	size_t Probe(const NameString& key) const
	{
		size_t mask = slots.size() - 1;
		size_t i = ((uint32_t)key.GetCompareIndex() * 2654435761u) & mask;
		while (slots[i].used && slots[i].first != key) i = (i + 1) & mask;
		return i;
	}
	void Grow()
	{
		Array<Slot> old = std::move(slots);
		slots = Array<Slot>();
		slots.resize(old.empty() ? 64 : old.size() * 2);
		for (Slot& s : old)
			if (s.used) slots[Probe(s.first)] = s;
	}
	Array<Slot> slots;
	size_t count = 0;
};
#endif
