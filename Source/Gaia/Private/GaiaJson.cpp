// Copyright VChuckShunA. All Rights Reserved.

#include "GaiaJson.h"

#if GAIA_ENABLED

#include "Serialization/JsonWriter.h"

namespace Gaia
{

FJsonFields& FJsonFields::Add(const TCHAR* Key, const FString& Value)
{
	AddKey(Key);
	AppendEscapeJsonString(Text, Value);
	return *this;
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, const TCHAR* Value)
{
	return Add(Key, FString(Value));
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, FName Value)
{
	if (Value.IsNone())
	{
		return AddRaw(Key, TEXT("null"));
	}
	return Add(Key, Value.ToString());
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, bool bValue)
{
	return AddRaw(Key, bValue ? TEXT("true") : TEXT("false"));
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, int32 Value)
{
	AddKey(Key);
	Text.AppendInt(Value);
	return *this;
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, int64 Value)
{
	AddKey(Key);
	Text.Appendf(TEXT("%lld"), Value);
	return *this;
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, double Value)
{
	if (!FMath::IsFinite(Value))
	{
		return AddRaw(Key, TEXT("null"));
	}
	AddKey(Key);
	Text.Appendf(TEXT("%.2f"), Value);
	return *this;
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, const FVector& Value)
{
	AddKey(Key);
	Text.Appendf(TEXT("[%lld,%lld,%lld]"), FMath::RoundToInt64(Value.X), FMath::RoundToInt64(Value.Y), FMath::RoundToInt64(Value.Z));
	return *this;
}

FJsonFields& FJsonFields::Add(const TCHAR* Key, TConstArrayView<FName> Values)
{
	AddKey(Key);
	Text += TEXT('[');
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		if (Index > 0)
		{
			Text += TEXT(',');
		}
		AppendEscapeJsonString(Text, Values[Index].ToString());
	}
	Text += TEXT(']');
	return *this;
}

FJsonFields& FJsonFields::AddRaw(const TCHAR* Key, const FString& Json)
{
	AddKey(Key);
	Text += Json;
	return *this;
}

void FJsonFields::AddKey(const TCHAR* Key)
{
	if (bNeedsComma)
	{
		Text += TEXT(',');
	}
	AppendEscapeJsonString(Text, Key);
	Text += TEXT(':');
	bNeedsComma = true;
}

} // namespace Gaia

#endif // GAIA_ENABLED
