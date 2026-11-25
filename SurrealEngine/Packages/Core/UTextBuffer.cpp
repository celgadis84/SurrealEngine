
#include "Precomp.h"
#include "UTextBuffer.h"

void UTextBuffer::Load(ObjectStream* stream)
{
	UObject::Load(stream);

	Pos = stream->ReadUInt32();
	Top = stream->ReadUInt32();

	int size = stream->ReadIndex();
#ifdef __N64__
	// N64 port: script source text is only for the compiler / debugger (460 KB of Engine.u)
	if (size > 0)
	{
		stream->Seek(stream->Tell() + size);
		size = 0;
	}
#endif
	if (size > 0)
	{
		Text.resize(size);
		stream->ReadBytes(&Text[0], size);
		if (!Text.empty() && Text.back() == 0) Text.pop_back();
	}
}

void UTextBuffer::Save(PackageStreamWriter* stream)
{
	UObject::Save(stream);
	stream->WriteUInt32(Pos);
	stream->WriteUInt32(Top);
	stream->WriteIndex((int)Text.size() + 1);
	stream->WriteBytes(Text.c_str(), (uint32_t)Text.size() + 1);
}
