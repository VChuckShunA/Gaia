// Copyright VChuckShunA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaiaTypes.h"

#if GAIA_ENABLED

namespace Gaia
{
	/** Appends the fields of one JSON object to a buffer. Used on the worker thread, so it only handles plain data. */
	class GAIA_API FJsonFields
	{
	public:
		/** bHasFields: the object already has a field, so the first added here needs a comma. */
		explicit FJsonFields(FString& InText, bool bHasFields = false)
			: Text(InText)
			, bNeedsComma(bHasFields)
		{
		}

		FJsonFields(const FJsonFields&) = delete;
		FJsonFields& operator=(const FJsonFields&) = delete;

		FJsonFields& Add(const TCHAR* Key, const FString& Value);
		FJsonFields& Add(const TCHAR* Key, const TCHAR* Value);
		/** None is written as null. */
		FJsonFields& Add(const TCHAR* Key, FName Value);
		FJsonFields& Add(const TCHAR* Key, bool bValue);
		FJsonFields& Add(const TCHAR* Key, int32 Value);
		FJsonFields& Add(const TCHAR* Key, int64 Value);
		FJsonFields& Add(const TCHAR* Key, double Value);
		/** To the nearest centimetre, as [x,y,z]. */
		FJsonFields& Add(const TCHAR* Key, const FVector& Value);
		FJsonFields& Add(const TCHAR* Key, TConstArrayView<FName> Values);
		/** Json must already be valid JSON. */
		FJsonFields& AddRaw(const TCHAR* Key, const FString& Json);

	protected:
		FString& Text;

	private:
		void AddKey(const TCHAR* Key);

		bool bNeedsComma;
	};
}

#endif // GAIA_ENABLED
