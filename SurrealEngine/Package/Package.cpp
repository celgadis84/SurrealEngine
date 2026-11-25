
#include "Precomp.h"
#include "Package.h"
#include "PackageWriter.h"
#include "PackageStream.h"
#include "PackageManager.h"
#include "Utils/File.h"
#include "Packages/Core/UObject.h"
#include "Packages/Core/UClass.h"
extern "C" unsigned ue_heap_used(void); // DBG

Package::Package(PackageManager* packageManager, const NameString& name, const std::string& filepath) : Packages(packageManager), Name(name), FilePath(filepath)
{
	unsigned dbg0 = ue_heap_used();
	if (!filepath.empty())
#ifdef UE_K64TABLES
		if (!ReadTablesN64())
#endif
		ReadTables();
#ifdef UE_DBG_CENSUS
	printf("[ue] DBG pkg %s tables %u KB (n%d e%d i%d) used %u KB\n", filepath.c_str(), (ue_heap_used() - dbg0) >> 10, (int)NameTable.size(), (int)ExportTable.size(), (int)ImportTable.size(), ue_heap_used() >> 10);
#endif

	FileName = fs::path(FilePath).filename().string();
	FileExtension = fs::path(FilePath).extension().string();

	// Register native classes not listed in the package
	for (const auto& it : packageManager->NativeClasses)
	{
		const NativeClass& nativeClass = it.second;
		if (nativeClass.Package == name)
		{
			int objref = FindObjectReference("Class", nativeClass.Name, {});
			if (objref == 0)
			{
#ifdef __N64__
				int nativeName = -1;
#ifdef UE_K64TABLES
				for (int ni = 0; ni < NbN64 && nativeName < 0; ni++)
					if (GetName(ni) == nativeClass.Name) nativeName = ni;
				for (size_t ni = 0; ni < NameTable.size() && nativeName < 0; ni++)
					if (NameTable[ni].Name == nativeClass.Name) nativeName = NbN64 + (int)ni;
#else
				for (size_t ni = 0; ni < NameTable.size(); ni++)
					if (NameTable[ni].Name == nativeClass.Name) { nativeName = (int)ni; break; }
#endif
				if (nativeName < 0)
#else
				if (NameHash.find(nativeClass.Name) == NameHash.end())
#endif
				{
					NameTableEntry nameentry;
					nameentry.Flags = 0;
					nameentry.Name = nativeClass.Name;
					NameTable.push_back(nameentry);
#ifdef __N64__
#ifdef UE_K64TABLES
					nativeName = NbN64 + (int)NameTable.size() - 1;
#else
					nativeName = (int)NameTable.size() - 1;
#endif
#else
					NameHash[nativeClass.Name] = (int)NameTable.size() - 1;
#endif
				}

				ExportTableEntry entry;
				entry.ObjClass = 0;
				entry.ObjBase = nativeClass.Base.IsNone() ? 0 : FindObjectReference("Class", nativeClass.Base, {});
				entry.ObjOuter = 0;
#ifdef __N64__
				entry.ObjName = nativeName;
#else
				entry.ObjName = NameHash[nativeClass.Name];
#endif
				entry.ObjFlags = ObjectFlags::Native;
				entry.ObjSize = 0;
				entry.ObjOffset = 0;
				ExportTable.push_back(entry);
			}
		}
	}

#ifdef UE_K64TABLES
	ExportObjects.resize(ExportCountN64());
#else
	ExportObjects.resize(ExportTable.size());
#endif
}

Package::~Package()
{
#ifdef UE_K64TABLES
	free(NameIdsN64);
#endif
}

void Package::LoadAll()
{
#ifdef UE_K64TABLES
	for (int i = 0; i < ExportCountN64(); i++)
#else
	for (size_t i = 0; i < ExportTable.size(); i++)
#endif
	{
		GetUObject((int)i + 1);
	}
}

void Package::Save(UObject* object, const std::string& filename)
{
	if (object && object->package != this)
		Exception::Throw("Object does not belong to this package");

	PackageWriter writer(this);
	writer.Save(object, filename);
}

GCAllocation* Package::Mark(GCAllocation* marklist)
{
	for (UObject* obj : ExportObjects)
		marklist = GC::MarkObject(marklist, obj);
	return marklist;
}

UObject* Package::NewObject(const NameString& objname, UClass* objclass, ObjectFlags flags, bool initProperties)
{
	if (!objclass)
		Exception::Throw("Package.NewObject called with a null objclass!");

	for (UClass* cur = objclass; cur != nullptr; cur = static_cast<UClass*>(cur->BaseStruct))
	{
		auto it = Packages->NativeClasses.find(cur->Name);
		if (it != Packages->NativeClasses.end())
		{
			UObject* obj = it->second.CreateFunc(objname, objclass, flags);
			obj->package = this;
			if (initProperties)
			{
				obj->PropertyData.Init(objclass);
				obj->SetObject("Class", obj->Class);
				obj->SetName("Name", obj->Name);
				obj->SetInt("ObjectFlags", (int)obj->Flags);
			}
			return obj;
		}
	}

	Exception::Throw("Could not find the native class for " + objname.ToString());
}

void Package::LoadExportObject(int index)
{
#ifdef UE_K64TABLES
	const ExportTableEntry entryN64 = ExportN64(index);
	const ExportTableEntry* entry = &entryN64;
#else
	const ExportTableEntry* entry = &ExportTable[index];
#endif

	SetDelayLoadActive delayload(Packages);

	NameString objname = GetName(entry->ObjName);

	if (entry->ObjClass != 0)
	{
		UClass* objclass = UObject::Cast<UClass>(GetUObject(entry->ObjClass));
		if (!objclass)
		{
			Exception::Throw("Could not find the object class for " + objname.ToString());
		}

		UObject* outer = entry->ObjOuter != 0 ? GetUObject(entry->ObjOuter) : nullptr;
		ExportObjects[index] = NewObject(objname, objclass, entry->ObjFlags, false);
		ExportObjects[index]->DelayLoad.reset(new ObjectDelayLoad(this, index, objname, objclass, outer));
		Packages->delayLoads.push_back(ExportObjects[index]);
	}
	else
	{
		UClass* objbase = UObject::Cast<UClass>(GetUObject(entry->ObjBase));
		if (!objbase && objname != "Object")
			objbase = UObject::Cast<UClass>(Packages->GetPackage("Core")->GetUObject("Class", "Object"));
		auto obj = GC::Alloc<UClass>(objname, objbase, entry->ObjFlags);
		ExportObjects[index] = obj;
		ExportObjects[index]->DelayLoad.reset(new ObjectDelayLoad(this, index, objname, objbase, nullptr));
		Packages->delayLoads.push_back(ExportObjects[index]);
		obj->Class = UObject::Cast<UClass>(Packages->GetPackage("Core")->GetUObject("Class", "Class"));
	}
}

UObject* Package::GetUObject(int objref)
{
	if (objref > 0) // Export table object
	{
		int index = objref - 1;

		if ((size_t)index > ExportObjects.size())
		{
			printf("[ue] DBG bad objref %d exports %d pkg %s\n", objref, (int)ExportObjects.size(), FilePath.c_str());
			Exception::Throw("Invalid object reference");
		}

		if (!ExportObjects[index])
			LoadExportObject(index);

		if (Packages->delayLoadActive == 0)
			Packages->DelayLoadNow();

		UObject* object = ExportObjects[index];
		object->package = this;
		object->exportIndex = index;
		return object;
	}
	else if (objref < 0) // Import table object
	{
		ImportTableEntry* entry = GetImportEntry(objref);
		NameString objectName = GetName(entry->ObjName);
		NameString className = GetName(entry->ClassName);

		if (entry->ObjOuter == 0)
			return nullptr; // This is a root package object. We don't have an UObject for those currently

		// Find the path for the outer object (we can't just grab the object pointer due to UnrealI/UnrealShare)
		std::string group;
		ImportTableEntry* outerEntry = GetImportEntry(entry->ObjOuter);
		while (outerEntry->ObjOuter != 0)
		{
			if (!group.empty())
				group += '.';
			group += GetName(outerEntry->ObjName).ToString();
			outerEntry = GetImportEntry(outerEntry->ObjOuter);
		}

		NameString packageName = GetName(outerEntry->ObjName);

		UObject* obj = Packages->GetPackage(packageName)->GetUObject(className, objectName, group);
		if (!obj && packageName == "UnrealI")
			obj = Packages->GetPackage("UnrealShare")->GetUObject(className, objectName, group);
		else if (!obj && packageName == "UnrealShare")
			obj = Packages->GetPackage("UnrealI")->GetUObject(className, objectName, group);
		if (!obj && packageName != "Engine" && packageName != "Core")
			obj = Packages->GetPackage("Engine")->GetUObject(className, objectName, group);
		return obj;
	}
	else
	{
		return nullptr;
	}
}

UObject* Package::GetUObject(const NameString& className, const NameString& objectName, const NameString& group, bool ignoreGroup)
{
	return GetUObject(FindObjectReference(className, objectName, group, ignoreGroup));
}

UClass* Package::GetClass(const NameString& className)
{
	return UObject::Cast<UClass>(GetUObject("Class", className));
}

#ifdef UE_K64TABLES
// same search as the stock one below: names compared on the u16 ids, export records read from
// ROM (by value: GetUObject of a class may load more exports) only for the name matches
int Package::FindObjectReference(const NameString& className, const NameString& objectName, const NameString& group, bool ignoreGroup)
{
	bool isClass = className == "Class";

	int count = ExportCountN64();
	for (int index = 0; index < count; index++)
	{
		int nameIndex = index < NeN64 ? ExNamesN64[index] : ExportTable[index - NeN64].ObjName;
		if (GetName(nameIndex) != objectName)
			continue;
		const ExportTableEntry entry = ExportN64(index);

		if (!ignoreGroup)
		{
			std::string entryGroup;
			int outerRef = entry.ObjOuter;
			while (outerRef != 0)
			{
				if (!entryGroup.empty())
					entryGroup += '.';
				if (outerRef > 0)
				{
					const ExportTableEntry outerEntry = *GetExportEntry(outerRef);
					entryGroup += GetName(outerEntry.ObjName).ToString();
					outerRef = outerEntry.ObjOuter;
				}
				else
				{
					auto outerEntry = GetImportEntry(outerRef);
					entryGroup += GetName(outerEntry->ObjName).ToString();
					outerRef = outerEntry->ObjOuter;
				}
			}
			if ((group.IsNone() && !entryGroup.empty()) || (!group.IsNone() && group != entryGroup))
				continue;
		}

		if (isClass)
		{
			if (entry.ObjClass == 0)
			{
				return index + 1;
			}
			else if (entry.ObjClass < 0)
			{
				auto classImport = GetImportEntry(entry.ObjClass);
				if (classImport && className == GetName(classImport->ObjName))
					return index + 1;
			}
			else
			{
				int ci = entry.ObjClass + 1; // sic: the stock code reads ExportTable[ObjClass + 1]
				if (ci < count && className == GetName(ExportN64(ci).ObjName))
					return index + 1;
			}
		}
		else if (entry.ObjClass != 0)
		{
			UClass* cls = UObject::Cast<UClass>(GetUObject(entry.ObjClass));
			while (cls)
			{
				if (className == cls->Name)
					return index + 1;
				cls = static_cast<UClass*>(cls->BaseStruct);
			}
		}
	}

	return 0;
}
#else
int Package::FindObjectReference(const NameString& className, const NameString& objectName, const NameString& group, bool ignoreGroup)
{
	bool isClass = className == "Class";

	size_t count = ExportTable.size();
	for (size_t index = 0; index < count; index++)
	{
		ExportTableEntry& entry = ExportTable[index];
		if (GetName(entry.ObjName) != objectName)
			continue;

		if (!ignoreGroup)
		{
			// Find the path for the outer object
			std::string entryGroup;
			int outerRef = entry.ObjOuter;
			while (outerRef != 0)
			{
				if (!entryGroup.empty())
					entryGroup += '.';
				if (outerRef > 0)
				{
					auto outerEntry = GetExportEntry(outerRef);
					entryGroup += GetName(outerEntry->ObjName).ToString();
					outerRef = outerEntry->ObjOuter;
				}
				else// if (outerRef < 0)
				{
					auto outerEntry = GetImportEntry(outerRef);
					entryGroup += GetName(outerEntry->ObjName).ToString();
					outerRef = outerEntry->ObjOuter;
				}
			}
			if ((group.IsNone() && !entryGroup.empty()) || (!group.IsNone() && group != entryGroup))
				continue;
		}

		if (isClass)
		{
			if (entry.ObjClass == 0)
			{
				return (int)index + 1;
			}
			else if (entry.ObjClass < 0)
			{
				auto classImport = GetImportEntry(entry.ObjClass);
				if (classImport && className == GetName(classImport->ObjName))
					return (int)index + 1;
			}
			else
			{
				auto classExport = &ExportTable[entry.ObjClass + 1];
				if (classExport && className == GetName(classExport->ObjName))
					return (int)index + 1;
			}
		}
		else if (entry.ObjClass != 0)
		{
			UClass* cls = UObject::Cast<UClass>(GetUObject(entry.ObjClass));
			while (cls)
			{
				if (className == cls->Name)
					return (int)index + 1;
				cls = static_cast<UClass*>(cls->BaseStruct);
			}
		}
	}

	return 0;
}
#endif

#ifdef UE_K64TABLES
NameString Package::GetName(int index) const
{
	if (index >= 0 && index < NbN64)
		return NameString::FromPoolN64(NameIdsN64[index]);
	if (index >= NbN64 && (size_t)(index - NbN64) < NameTable.size())
		return NameTable[index - NbN64].Name;
	Exception::Throw("Name index out of bounds!: " + Name.ToString());
}
#else
const NameString& Package::GetName(int index) const
{
	if (index >= 0 && (size_t)index < NameTable.size())
		return NameTable[index].Name;
	else
		Exception::Throw("Name index out of bounds!: " + Name.ToString());
}
#endif

ExportTableEntry* Package::GetExportEntry(int objref)
{
	if (objref == 0)
		return nullptr;
	else if (objref < 0)
		Exception::Throw("Expected an export table entry: " + Name.ToString());

	int index = objref - 1;
#ifdef UE_K64TABLES
	if (index >= ExportCountN64())
		Exception::Throw("Export table entry out of bounds!: " + Name.ToString());

	ExportTableEntry* e = &RingN64[RingPosN64++ & 7];
	*e = ExportN64(index);
	return e;
#else
	if ((size_t)index >= ExportTable.size())
		Exception::Throw("Export table entry out of bounds!: " + Name.ToString());

	return ExportTable.data() + index;
#endif
}

ImportTableEntry* Package::GetImportEntry(int objref)
{
	if (objref == 0)
		return nullptr;
	else if (objref > 0)
		Exception::Throw("Expected an import table entry: " + Name.ToString());

	int index = -objref - 1;
	if ((size_t)index >= ImportTable.size())
	{
#ifdef __N64__
		printf("[ue] DBG bad import ref %d imports %d pkg %s\n", objref, (int)ImportTable.size(), FilePath.c_str());
#endif
		Exception::Throw("Import table entry out of bounds!: " + Name.ToString());
	}

	return ImportTable.data() + index;
}

void Package::ReadTables()
{
	auto stream = Packages->GetStream(this);
	stream->Seek(0);

	uint32_t signature = stream->ReadInt32();
	if (signature != 0x9E2A83C1)
		Exception::Throw("Not an unreal package file: " + Name.ToString());

	Version = stream->ReadInt16();
	LicenseeMode = stream->ReadInt16();

	if (Version < 60 || Version >= 100)
		Exception::Throw("Unsupported unreal package version: " + Name.ToString());

	Flags = (PackageFlags)stream->ReadUInt32();

	uint32_t nameCount = stream->ReadInt32();
	uint32_t nameOffset = stream->ReadInt32();

	uint32_t exportCount = stream->ReadInt32();
	uint32_t exportOffset = stream->ReadInt32();

	uint32_t importCount = stream->ReadInt32();
	uint32_t importOffset = stream->ReadInt32();

	if (Version < 68)
	{
		uint32_t heritageCount = stream->ReadInt32();
		uint32_t heritageOffset = stream->ReadInt32();
	}
	else
	{
		stream->ReadBytes(Guid, 16);
		uint32_t generationCount = stream->ReadInt32();
		for (uint32_t i = 0; i < generationCount; i++)
		{
			uint32_t genExportCount = stream->ReadInt32();
			uint32_t genNameCount = stream->ReadInt32();
		}
	}

	stream->Seek(nameOffset);
#ifdef __N64__
	// N64 port: exact capacity (+ native classes the constructor appends)
	uint32_t nativeCount = 0;
	for (const auto& it : Packages->NativeClasses)
		if (it.second.Package == Name)
			nativeCount++;
	NameTable.reserve(nameCount + nativeCount);
	// N64: sin NameHash por paquete (el constructor busca en NameTable)
	ExportTable.reserve(exportCount + nativeCount);
	ImportTable.reserve(importCount);
#endif
	for (uint32_t i = 0; i < nameCount; i++)
	{
		NameTableEntry entry;
		entry.Name = stream->ReadString();
		entry.Flags = stream->ReadInt32();
		NameTable.push_back(entry);
#ifndef __N64__
		NameHash[entry.Name] = i;
#endif
	}

	stream->Seek(exportOffset);
	for (uint32_t i = 0; i < exportCount; i++)
	{
		ExportTableEntry entry;
		entry.ObjClass = stream->ReadIndex();
		entry.ObjBase = stream->ReadIndex();
		entry.ObjOuter = stream->ReadInt32();
		entry.ObjName = stream->ReadIndex();
		entry.ObjFlags = (ObjectFlags)stream->ReadInt32();
		if ((uint32_t)entry.ObjFlags & 0x100) stream->ReadInt32(); // 227: extra field
		entry.ObjSize = stream->ReadIndex();
		entry.ObjOffset = (entry.ObjSize > 0) ? stream->ReadIndex() : -1;
		ExportTable.push_back(entry);
	}

	stream->Seek(importOffset);
	for (uint32_t i = 0; i < importCount; i++)
	{
		ImportTableEntry entry;
		entry.ClassPackage = stream->ReadIndex();
		entry.ClassName = stream->ReadIndex();
		entry.ObjOuter = stream->ReadInt32();
		entry.ObjName = stream->ReadIndex();
		ImportTable.push_back(entry);
	}
}

#ifdef __N64__
#ifdef UE_K64TABLES
extern "C" {
#include <k64/romfs.h>
}

bool Package::ReadTablesN64()
{
	if (!NameString::PoolCountN64())
		return false;
	k64_file_t f;
	if (!k64_romfs_open(&f, (FilePath + ".k64pt").c_str()) || f.packed)
	{
		printf("[ue] k64pt: falta %s.k64pt (ReadTables)\n", FilePath.c_str());
		return false;
	}
	alignas(8) uint32_t h[12];
	if (k64_romfs_read(&f, h, 44) != 44 || h[0] != 0x4B365054) // 'K6PT'
		return false;
	Version = (int)(h[1] >> 16);
	LicenseeMode = (int)(h[1] & 0xffff);
	Flags = (PackageFlags)h[2];
	uint32_t nn = h[3], ne = h[4], ni = h[5], offNames = h[6], offExn = h[7], offImp = h[8], offEx = h[9];
	uint32_t bytes = offImp - offNames;
	NameIdsN64 = (uint16_t*)malloc(bytes + 8);
	k64_romfs_seek(&f, offNames);
	if (!NameIdsN64 || k64_romfs_read(&f, NameIdsN64, bytes) != bytes)
		Exception::Throw("k64pt: lectura fallida: " + Name.ToString());
	ExNamesN64 = NameIdsN64 + (offExn - offNames) / 2;
	ImportTable.resize(ni);
	k64_romfs_seek(&f, offImp);
	if (ni && k64_romfs_read(&f, ImportTable.data(), ni * 16) != ni * 16)
		Exception::Throw("k64pt: lectura fallida: " + Name.ToString());
	ExRomN64 = f.rom + offEx;
	NbN64 = (int)nn;
	NeN64 = (int)ne;
	uint32_t nativeCount = 0;
	for (const auto& it : Packages->NativeClasses)
		if (it.second.Package == Name)
			nativeCount++;
	NameTable.reserve(nativeCount);
	ExportTable.reserve(nativeCount);
	return true;
}

// export records in ROM: lines of 8 (160 B), 64-line direct-mapped cache for all packages
alignas(16) static uint8_t ExRecN64[64][160];
static const Package* ExPkgN64[64];
static int ExLineN64[64];

static inline uint32_t ExRd32N64(const uint8_t* r) { return ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3]; }

ExportTableEntry Package::ExportN64(int index) const
{
	if (index >= NeN64)
		return ExportTable[index - NeN64];
	int line = index >> 3;
	unsigned slot = ((unsigned)(uintptr_t)this / 16 + (unsigned)line) & 63;
	if (ExPkgN64[slot] != this || ExLineN64[slot] != line)
	{
		int first = line << 3;
		int cnt = NeN64 - first < 8 ? NeN64 - first : 8;
		k64_file_t f;
		f.rom = ExRomN64;
		f.size = (uint32_t)NeN64 * 20;
		f.pos = (uint32_t)first * 20;
		f.packed = false;
		k64_romfs_read(&f, ExRecN64[slot], (uint32_t)cnt * 20);
		ExPkgN64[slot] = this;
		ExLineN64[slot] = line;
	}
	const uint8_t* r = ExRecN64[slot] + (index & 7) * 20;
	ExportTableEntry e;
	e.ObjClass = (int16_t)((r[0] << 8) | r[1]);
	e.ObjBase = (int16_t)((r[2] << 8) | r[3]);
	e.ObjOuter = (int16_t)((r[4] << 8) | r[5]);
	e.ObjName = (r[6] << 8) | r[7];
	e.ObjFlags = (ObjectFlags)ExRd32N64(r + 8);
	e.ObjSize = (int32_t)ExRd32N64(r + 12);
	e.ObjOffset = (int32_t)ExRd32N64(r + 16);
	return e;
}
#endif

void Package::ReadFileBytes(uint32_t fileoffset, void* dst, uint32_t len)
{
	auto stream = Packages->GetStream(this);
	stream->Seek(fileoffset);
	stream->ReadBytes(dst, len);
}
#endif

std::unique_ptr<ObjectStream> Package::OpenObjectStream(int index, const NameString& name, UClass* base)
{
#ifdef UE_K64TABLES
	const ExportTableEntry entry = ExportN64(index);
#else
	const auto& entry = ExportTable[index];
#endif
	if (entry.ObjSize > 0)
	{
#ifdef __N64__
		if (entry.ObjSize > 262144)
		{
			// big export: 64 KB window refilled from the file (ObjectStream lazy mode)
			std::unique_ptr<uint64_t[]> wbuf(new uint64_t[65536 / 8]);
			auto os = std::make_unique<ObjectStream>(this, std::move(wbuf), entry.ObjOffset, entry.ObjSize, name, base);
			os->SetLazy();
			return os;
		}
#endif
		std::unique_ptr<uint64_t[]> buffer(new uint64_t[(entry.ObjSize + 7) / 8]);
		auto stream = Packages->GetStream(this);
		stream->Seek(entry.ObjOffset);
		stream->ReadBytes(buffer.get(), entry.ObjSize);
		return std::make_unique<ObjectStream>(this, std::move(buffer), entry.ObjOffset, entry.ObjSize, name, base);
	}
	else
	{
		return std::make_unique<ObjectStream>(this, std::unique_ptr<uint64_t[]>(), 0, 0, name, base);
	}
}

#ifdef UE_K64TABLES
std::string Package::GetExportName(int objref)
{
	if (objref <= 0)
		return "None";

	ExportTableEntry entry = ExportN64(objref); // sic: the stock code indexes ExportTable[objref]
	std::string objname = GetName(entry.ObjName).ToString();

	while (entry.ObjOuter != 0)
	{
		entry = ExportN64(entry.ObjOuter - 1);
		objname = GetName(entry.ObjName).ToString() + '.' + objname;
	}

	objname = Name.ToString() + '.' + objname;
	return objname;
}
#else
std::string Package::GetExportName(int objref)
{
	if (objref <= 0)
		return "None";

	ExportTableEntry& entry = ExportTable[objref];
	std::string objname = NameTable[entry.ObjName].Name.ToString();

	while (entry.ObjOuter != 0)
	{
		entry = ExportTable[entry.ObjOuter - 1];
		objname = NameTable[entry.ObjName].Name.ToString() + '.' + objname;
	}

	objname = Name.ToString() + '.' + objname;
	return objname;
}
#endif
