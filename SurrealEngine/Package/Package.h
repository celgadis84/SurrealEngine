#pragma once

#include "GC/GC.h"
#include "PackageFlags.h"
#include "PackageTables.h"
#include "ObjectFlags.h"
#include "NameString.h"
#include <functional>

class PackageManager;
class PackageStream;
class ObjectStream;
class UObject;
class UClass;

class Package : public GCObject
{
public:
	Package(PackageManager* packageManager, const NameString& name, const std::string& filepath);
	~Package();

	UObject* NewObject(const NameString& objname, UClass* objclass, ObjectFlags flags, bool initProperties = true);

	UObject* GetUObject(int objref);
	UObject* GetUObject(const NameString& className, const NameString& objectName) { return GetUObject(className, objectName, {}, true); }
	UObject* GetUObject(const NameString& className, const NameString& objectName, const NameString& group, bool ignoreGroup = false);

	UClass* GetClass(const NameString& className);

	void LoadAll();
	void Save(UObject* object = nullptr, const std::string& filename = {});

#ifdef UE_K64TABLES
	NameString GetName(int index) const; // by value: package names are u16 pool ids
#else
	const NameString& GetName(int index) const;
#endif
	int GetVersion() const { return Version; }
	NameString GetPackageName() const { return Name; }
	std::string GetPackageFileName() const { return FileName; }
	std::string GetPackageFilePath() const { return FilePath; }
	std::string GetPackageFileExtension() const { return FileExtension; }

	PackageManager* GetPackageManager() { return Packages; }

	ExportTableEntry* GetExportEntry(int objref);
	ImportTableEntry* GetImportEntry(int objref);
	int FindObjectReference(const NameString& className, const NameString& objectName, const NameString& group, bool ignoreGroup = false);

	std::string GetExportName(int objref);

	template<class T> Array<T*> GetAllObjects();

#ifdef __N64__
	// UStruct::LazyCode re-reads a function's bytecode from its export
	std::unique_ptr<ObjectStream> OpenExportStream(int index, const NameString& name, UClass* base) { return OpenObjectStream(index, name, base); }
	void ReadFileBytes(uint32_t fileoffset, void* dst, uint32_t len);
#endif

private:
	GCAllocation* Mark(GCAllocation* marklist) override;

	void ReadTables();
	std::unique_ptr<ObjectStream> OpenObjectStream(int index, const NameString& name, UClass* base);
	void LoadExportObject(int index);

	PackageManager* Packages = nullptr;
	NameString Name;
	std::string FilePath;
	std::string FileName;
	std::string FileExtension;

	int Version = 0;
	int LicenseeMode = 0;
	PackageFlags Flags = PackageFlags::NoFlags;
	Array<NameTableEntry> NameTable;
	Array<ExportTableEntry> ExportTable;
	Array<ImportTableEntry> ImportTable;
	uint8_t Guid[16] = {};

#ifdef __N64__
	NameIndexMap NameHash;
#else
	std::map<NameString, int> NameHash;
#endif

	Array<UObject*> ExportObjects;

#ifdef UE_K64TABLES
	// N64 port: tables baked offline (baker/unrtab.py -> <package>.k64pt). Names: u16 pool id per
	// package name, NameTable keeps only the names appended at run time (index NbN64 + i).
	// Exports: 20 B records left in ROM, read by value through a small cache (ExportN64);
	// ExportTable keeps only the native classes appended at run time (index NeN64 + i).
	// GetExportEntry hands out one of 8 ring copies: use it at once, never keep the pointer.
	bool ReadTablesN64();
	ExportTableEntry ExportN64(int index) const;
	int ExportCountN64() const { return NeN64 + (int)ExportTable.size(); }
	uint16_t* NameIdsN64 = nullptr;
	const uint16_t* ExNamesN64 = nullptr;
	uint32_t ExRomN64 = 0;
	int NbN64 = 0;
	int NeN64 = 0;
	ExportTableEntry RingN64[8];
	unsigned RingPosN64 = 0;
#endif

	Package(const Package&) = delete;
	Package& operator=(const Package&) = delete;

	friend class PackageManager;
	friend class UObject;
	friend class PackageWriter;
};
