
// ****************************************************************************
// File: RTTI.cpp
// Desc: Run-Time Type Information (RTTI) support
//
// ****************************************************************************
#include "stdafx.h"
#include "Main.h"
#include "RTTI.h"
#include "Vftable.h"

// const Name::`vftable'
static LPCSTR FORMAT_RTTI_VFTABLE = "??_7%s6B@";
static LPCSTR FORMAT_RTTI_VFTABLE_PREFIX = "??_7";
// type 'RTTI Type Descriptor'
static LPCSTR FORMAT_RTTI_TYPE = "??_R0?%s@8";
// 'RTTI Base Class Descriptor at (a,b,c,d)'
static LPCSTR FORMAT_RTTI_BCD = "??_R1%s%s%s%s%s8";
// `RTTI Base Class Array'
static LPCSTR FORMAT_RTTI_BCA = "??_R2%s8";
// 'RTTI Class Hierarchy Descriptor'
static LPCSTR FORMAT_RTTI_CHD = "??_R3%s8";
// 'RTTI Complete Object Locator'
static LPCSTR FORMAT_RTTI_COL = "??_R4%s6B@";
static LPCSTR FORMAT_RTTI_COL_PREFIX = "??_R4";

// Skip type_info tag for class/struct mangled name strings
#define SKIP_TD_TAG(_str) ((_str) + SIZESTR(".?Ax"))

// Class name list container
struct bcdInfo
{
    char m_name[496];
    UINT m_attribute;
	RTTI::PMD m_pmd;
};
typedef qvector<bcdInfo> bcdList;

namespace RTTI
{
    void getBCDInfo(ea_t col, __out bcdList &nameList, __out UINT &numBaseClasses);
};


typedef std::unordered_map<ea_t, qstring> stringMap;
static stringMap stringCache;
static eaSet tdSet;
static eaSet chdSet;
static eaSet bcdSet;

void RTTI::freeWorkingData()
{
    stringCache.clear();
    tdSet.clear();
    chdSet.clear();
    bcdSet.clear();
}

// Mangle number for labeling
static LPSTR mangleNumber(UINT number, __out_bcount(64) LPSTR buffer)
{
	//
	// 0 = A@
	// X = X-1 (1 <= X <= 10)
	// -X = ? (X - 1)
	// 0x0..0xF = 'A'..'P'

	// Can only get unsigned inputs
	int num = *((PINT) &number);
	if(num == 0)
		return("A@");
	else
	{
		int sign = 0;
		if(num < 0)
		{
			sign = 1;
			num = -num;
		}

		if(num <= 10)
		{
			_snprintf_s(buffer, 64, (64 - 1), "%s%d", (sign ? "?" : ""), (num - 1));
			return(buffer);
		}
		else
		{
			// Count digits
			char buffer2[64] = {0};
			int  count = sizeof(buffer2);

			while((num > 0) && (count > 0))
			{
				buffer2[sizeof(buffer2) - count] = ('A' + (num % 16));
				num = (num / 16);
				count--;
			};

			if(count == 0)
				msg(" *** mangleNumber() overflow! ***");

			_snprintf_s(buffer, 64, (64-1), "%s%s@", (sign ? "?" : ""), buffer2);
			return(buffer);
		}
	}
}


// Return a short label indicating the CHD inheritance type by attributes
// TODO: Consider CHD_AMBIGUOUS?
static LPCSTR attributeLabel(UINT attributes)
{
    if ((attributes & 3) == RTTI::CHD_MULTINH)
		return("[MI]");
	else
    if ((attributes & 3) == RTTI::CHD_VIRTINH)
		return("[VI]");
	else
    if ((attributes & 3) == (RTTI::CHD_MULTINH | RTTI::CHD_VIRTINH))
		return("[MI VI]");
    else
        return("");
}


// Add RTTI definitions to IDA
// Structure type IDs
static tid_t s_type_info_ID = 1;
static tid_t s_ClassHierarchyDescriptor_ID = 2;
static tid_t s_PMD_ID = 3;
static tid_t s_BaseClassDescriptor_ID = 4;
static tid_t s_CompleteObjectLocator_ID = 5;

void RTTI::addDefinitionsToIda()
{
    char *lpszDecl;

#if IDA_SDK_VERSION >= 900
#define get_named_type_tid get_named_type_tid
#else
#define get_named_type_tid(name) import_type(NULL, -1, name)
#endif

	// IDA 7 has a definition for this now
	s_type_info_ID = get_named_type_tid("TypeDescriptor");
#if IDA_SDK_VERSION >= 900
    if (s_type_info_ID == BADADDR)
    {
        tinfo_t tif = tinfo_t();
        if (tif.get_named_type("TypeDescriptor"))
        {
            s_type_info_ID = tif.force_tid();
        }
    }
#endif
	if (s_type_info_ID == BADADDR)
	{
		msg("** Failed to load the IDA TypeDescriptor type, generating one **\n");

        if (!isDatabase64Bit)
        {
            lpszDecl = R"DECL(
                /// RTTI std::type_info class (#classinformer)
                struct type_info
                {
                    int vfptr;
                    int data;
                    char name[];
                };
            )DECL";
        }
        else
        {
            lpszDecl = R"DECL(
                /// RTTI std::type_info class (#classinformer)
                struct type_info
                {
                    void *vfptr;
                    void *data;
                    char name[];
                };
            )DECL";
        }
        if (parse_decls(nullptr, lpszDecl, msg, HTI_DCL) != 0)
            msg("** addDefinitionsToIda():  type_info failed! \n");
        s_type_info_ID = get_named_type_tid("type_info");
	}

    // Set the representation of the "name" field to a string literal.
#if IDA_SDK_VERSION >= 900
    if (s_type_info_ID != BADADDR)
    {
        tinfo_t tif = tinfo_t();
        value_repr_t repr = value_repr_t();
        if (tif.get_type_by_tid(s_type_info_ID) && repr.parse_value_repr("__strlit(C)"))
        {
            tif.set_udm_repr(2, repr);
        }
    }
#else
    struc_t *sptr = get_struc(s_type_info_ID);
    if (sptr)
    {
        member_t* mptr = get_member_by_name(sptr, "name");
        if (mptr)
        {
#define FF_STRLIT 0x50000000
            mptr->flag |= FF_STRLIT;
        }
    }
#endif

    // Must come before the following  "_RTTIBaseClassDescriptor"
    lpszDecl = R"DECL(
        /// RTTI Base class descriptor displacement container (#classinformer)
        struct _PMD
        {
            int mdisp;
            int pdisp;
            int vdisp;
        };
    )DECL";
    parse_decls(nullptr, lpszDecl, msg, HTI_DCL);
    s_PMD_ID = get_named_type_tid("_PMD");

    if (!isDatabase64Bit)
    {
        lpszDecl = R"DECL(
            /// RTTI Class Hierarchy Descriptor (#classinformer)
            struct _RTTIClassHierarchyDescriptor
            {
                int signature;
                int attributes;
                int numBaseClasses;
                void* baseClassArray;
            };
        )DECL";

    }
    else
    {
        lpszDecl = R"DECL(
            /// RTTI Class Hierarchy Descriptor (#classinformer)
            struct _RTTIClassHierarchyDescriptor
            {
                int signature;
                int attributes;
                int numBaseClasses;
                int baseClassArray;
            };
        )DECL";
    }
    parse_decls(nullptr, lpszDecl, msg, HTI_DCL);
    s_ClassHierarchyDescriptor_ID = get_named_type_tid("_RTTIClassHierarchyDescriptor");

    if (!isDatabase64Bit)
    {
        lpszDecl = R"DECL(
            /// RTTI Base Class Descriptor (#classinformer)
            struct _RTTIBaseClassDescriptor
            {
                void* typeDescriptor;
                int numContainedBases;
                _PMD pmd;
                int attributes;
            };
        )DECL";
    }
    else
    {
        lpszDecl = R"DECL(
            /// RTTI Base Class Descriptor (#classinformer)
            struct _RTTIBaseClassDescriptor
            {
                int typeDescriptor;
                int numContainedBases;
                _PMD pmd;
                int attributes;
            };
        )DECL";
    }
    parse_decls(nullptr, lpszDecl, msg, HTI_DCL);
    s_BaseClassDescriptor_ID = get_named_type_tid("_RTTIBaseClassDescriptor");

    if (!isDatabase64Bit)
    {
        lpszDecl = R"DECL(
            /// RTTI Complete Object Locator (#classinformer)
            struct _RTTICompleteObjectLocator
            {
                int signature;
                int offset;
                int cdOffset;
                void* typeDescriptor;
                void* classDescriptor;
            };
        )DECL";
    }
    else
    {
        lpszDecl = R"DECL(
            /// RTTI Complete Object Locator (#classinformer)
            struct _RTTICompleteObjectLocator
            {
                int signature;
                int offset;
                int cdOffset;
                int typeDescriptor;
                int classDescriptor;
                int objectBase;
            };
        )DECL";
    }
    parse_decls(nullptr, lpszDecl, msg, HTI_DCL);
    s_CompleteObjectLocator_ID = get_named_type_tid("_RTTICompleteObjectLocator");

#undef get_named_type_tid
}

// Version 1.05, manually set fields and then try to place the struct
// If it fails at least the fields should be set
// 2.5: IDA 7 now has RTTI support; only place structs if don't exist at address
// Returns TRUE if structure was placed, else it was already set
static BOOL tryStructRTTI(ea_t ea, tid_t tid, __in_opt LPSTR typeName = NULL, BOOL bHasChd = FALSE)
{
	#define putDword(ea) create_dword(ea, sizeof(DWORD))
    #define putEa(ea) isDatabase64Bit ? create_qword(ea, EA_SIZE) : create_dword(ea, EA_SIZE)

    UINT sizeof_RTTI_RTTICompleteObjectLocator = isDatabase64Bit ? sizeof(RTTI::_RTTICompleteObjectLocator) : sizeof(RTTI::_RTTICompleteObjectLocator32);

	if(tid == s_type_info_ID)
	{
        UINT offsetof_RTTI_type_info_M_d_name = isDatabase64Bit ? offsetof(RTTI::type_info64, _M_d_name) : offsetof(RTTI::type_info32, _M_d_name);
        UINT offsetof_RTTI_type_info_vfptr = isDatabase64Bit ? offsetof(RTTI::type_info64, vfptr) : offsetof(RTTI::type_info32, vfptr);
        UINT offsetof_RTTI_type_info_M_data = isDatabase64Bit ? offsetof(RTTI::type_info64, _M_data) : offsetof(RTTI::type_info32, _M_data);

		if (optionPlaceAtNamed || !hasName(ea))
		{
			_ASSERT(typeName != NULL);
			UINT nameLen = (UINT)(strlen(typeName) + 1);
			UINT structSize = (offsetof_RTTI_type_info_M_d_name + nameLen);

			// Place struct
			setUnknown(ea, structSize);
			BOOL result = FALSE;
			if (optionPlaceStructs)
				result = create_struct(ea, structSize, s_type_info_ID);
			if (!result)
			{
				putEa(ea + offsetof_RTTI_type_info_vfptr);
				putEa(ea + offsetof_RTTI_type_info_M_data);

				create_strlit((ea + offsetof_RTTI_type_info_M_d_name), nameLen, STRTYPE_C);
			}

			// sh!ft: End should be aligned
			ea_t end = (ea + offsetof_RTTI_type_info_M_d_name + nameLen);
            if (end % EA_SIZE)
                create_align(end, (EA_SIZE - (end % EA_SIZE)), 0);

			return TRUE;
		}
	}
	else
	if (tid == s_ClassHierarchyDescriptor_ID)
	{
		if (optionPlaceAtNamed || !hasName(ea))
		{
			setUnknown(ea, sizeof(RTTI::_RTTIClassHierarchyDescriptor));
			BOOL result = FALSE;
			if (optionPlaceStructs)
				result = create_struct(ea, sizeof(RTTI::_RTTIClassHierarchyDescriptor), s_ClassHierarchyDescriptor_ID);
			if (!result)
			{
				putDword(ea + offsetof(RTTI::_RTTIClassHierarchyDescriptor, signature));
				putDword(ea + offsetof(RTTI::_RTTIClassHierarchyDescriptor, attributes));
				putDword(ea + offsetof(RTTI::_RTTIClassHierarchyDescriptor, numBaseClasses));
				putDword(ea + offsetof(RTTI::_RTTIClassHierarchyDescriptor, baseClassArray));
			}

			return TRUE;
		}
	}
	else
	if(tid == s_PMD_ID)
	{
		if (optionPlaceAtNamed || !hasName(ea))
		{
			setUnknown(ea, sizeof(RTTI::PMD));
			BOOL result = FALSE;
			if (optionPlaceStructs)
				result = create_struct(ea, sizeof(RTTI::PMD), s_PMD_ID);
			if (!result)
			{
				putDword(ea + offsetof(RTTI::PMD, mdisp));
				putDword(ea + offsetof(RTTI::PMD, pdisp));
				putDword(ea + offsetof(RTTI::PMD, vdisp));
			}

			return TRUE;
		}
	}
	else
	if(tid == s_CompleteObjectLocator_ID)
	{
		if (optionPlaceAtNamed || !hasName(ea))
		{
			setUnknown(ea, sizeof_RTTI_RTTICompleteObjectLocator);
			BOOL result = FALSE;
			if (optionPlaceStructs)
				result = create_struct(ea, sizeof_RTTI_RTTICompleteObjectLocator, s_CompleteObjectLocator_ID);
			if (!result)
			{
				putDword(ea + offsetof(RTTI::_RTTICompleteObjectLocator, signature));
				putDword(ea + offsetof(RTTI::_RTTICompleteObjectLocator, offset));
				putDword(ea + offsetof(RTTI::_RTTICompleteObjectLocator, cdOffset));

				putDword(ea + offsetof(RTTI::_RTTICompleteObjectLocator, typeDescriptor));
				putDword(ea + offsetof(RTTI::_RTTICompleteObjectLocator, classDescriptor));
				if (isDatabase64Bit)
				{
					putDword(ea + offsetof(RTTI::_RTTICompleteObjectLocator, objectBase));
				}
            }

			return TRUE;
		}
	}
	else
	if (tid == s_BaseClassDescriptor_ID)
	{
		// Recursive
		tryStructRTTI(ea + offsetof(RTTI::_RTTIBaseClassDescriptor, pmd), s_PMD_ID);

		if (optionPlaceAtNamed || !hasName(ea))
		{
			setUnknown(ea, sizeof(RTTI::_RTTIBaseClassDescriptor));

			BOOL result = FALSE;
			if (optionPlaceStructs)
				result = create_struct(ea, sizeof(RTTI::_RTTIBaseClassDescriptor), s_BaseClassDescriptor_ID);
			if (!result)
			{
				putDword(ea + offsetof(RTTI::_RTTIBaseClassDescriptor, typeDescriptor));

				putDword(ea + offsetof(RTTI::_RTTIBaseClassDescriptor, numContainedBases));
				putDword(ea + offsetof(RTTI::_RTTIBaseClassDescriptor, attributes));
				if (bHasChd)
				{
					putDword(ea + (offsetof(RTTI::_RTTIBaseClassDescriptor, attributes) + sizeof(UINT)));
				}
			}

			return TRUE;
		}
	}
	else
	{
		_ASSERT(FALSE);
	}

	return FALSE;
}


// Read ASCII string from IDB at address
static int getIdaString(ea_t ea, __out LPSTR buffer, int bufferSize)
{
	buffer[0] = 0;

    // Return cached name if it exists
    stringMap::iterator it = stringCache.find(ea);
    if (it != stringCache.end())
    {
        LPCSTR str = it->second.c_str();
        int len = (int) strlen(str);
        if (len > bufferSize)
			len = bufferSize;
        strncpy_s(buffer, MAXSTR, str, len);
        return len;
    }
    else
    {
        // Read string at ea if it exists
        int len = (int) get_max_strlit_length(ea, STRTYPE_C, ALOPT_IGNHEADS);
        if (len > 0)
        {
			// Length includes terminator
            if (len > bufferSize)
				len = bufferSize;

			qstring str;
			int len2 = get_strlit_contents(&str, ea, len, STRTYPE_C);
            if (len2 > 0)
            {
				// Length with out terminator
				if (len2 > bufferSize)
					len2 = bufferSize;

                // Cache it
				memcpy(buffer, str.c_str(), len2);
                buffer[len2] = 0;
                stringCache[ea] = buffer;
            }
            else
                len = 0;
        }

        return len ;
    }
}


// --------------------------- Type descriptor ---------------------------

// Get type name into a buffer
// type_info assumed to be valid
int RTTI::type_info::getName(ea_t typeInfo, __out LPSTR buffer, int bufferSize)
{
    UINT offsetof_type_info_M_d_name = isDatabase64Bit ? offsetof(type_info64, _M_d_name) : offsetof(type_info32, _M_d_name);
    return(getIdaString(typeInfo + offsetof_type_info_M_d_name, buffer, bufferSize));
}

// A valid type_info/TypeDescriptor at pointer?
BOOL RTTI::type_info::isValid(ea_t typeInfo)
{
    // TRUE if we've already seen it
    if (tdSet.find(typeInfo) != tdSet.end())
        return(TRUE);

    if (is_loaded(typeInfo))
	{
        UINT offsetof_type_info_vfptr = isDatabase64Bit ? offsetof(type_info64, vfptr) : offsetof(type_info32, vfptr);
        UINT offsetof_type_info_M_data = isDatabase64Bit ? offsetof(type_info64, _M_data) : offsetof(type_info32, _M_data);
        UINT offsetof_type_info_M_d_name = isDatabase64Bit ? offsetof(type_info64, _M_d_name) : offsetof(type_info32, _M_d_name);

		// Verify what should be a vftable
        ea_t ea = getEa(typeInfo + offsetof_type_info_vfptr);
        if (is_loaded(ea))
		{
            // _M_data should be NULL statically
            ea_t _M_data = BADADDR;
            if (getVerifyEa((typeInfo + offsetof_type_info_M_data), _M_data))
            {
                if (_M_data == 0)
                    return(isTypeName(typeInfo + offsetof_type_info_M_d_name));
            }
		}
	}

	return(FALSE);
}

// Returns TRUE if known typename at address
BOOL RTTI::type_info::isTypeName(ea_t name)
{
    // Should start with a period
    if (get_byte(name) == '.')
    {
        // Read the rest of the possible name string
        char buffer[MAXSTR];
        if (getIdaString(name, buffer, SIZESTR(buffer)))
        {
            // Should be valid if it properly demangles
            if (LPSTR s = __unDName(NULL, buffer+1 /*skip the '.'*/, 0, mallocWrap, free, (UNDNAME_32_BIT_DECODE | UNDNAME_TYPE_ONLY)))
            {
                free(s);
                return(TRUE);
            }
        }
    }
    return(FALSE);
}

// Put struct and place name at address
void RTTI::type_info::tryStruct(ea_t typeInfo)
{
	// Only place once per address
	if (tdSet.find(typeInfo) != tdSet.end())
		return;
	else
		tdSet.insert(typeInfo);

	// Get type name
	char name[MAXSTR];
	int nameLen = getName(typeInfo, name, SIZESTR(name));

	tryStructRTTI(typeInfo, s_type_info_ID, name);

	if (nameLen > 0)
	{
		if (!hasName(typeInfo))
		{
			// Set decorated name/label
			char name2[MAXSTR];
			_snprintf_s(name2, sizeof(name2), SIZESTR(name2), FORMAT_RTTI_TYPE, (name + 2));
			setName(typeInfo, name2);
		}
	}
	else
	{
		_ASSERT(FALSE);
	}
}


// --------------------------- Complete Object Locator ---------------------------

// Return TRUE if address is a valid RTTI structure
BOOL RTTI::_RTTICompleteObjectLocator::isValid(ea_t col)
{
    if (is_loaded(col))
    {
        // Check signature
        UINT signature = -1;
        if (getVerify32((col + offsetof(_RTTICompleteObjectLocator, signature)), signature))
        {
            if (!isDatabase64Bit && signature == 0)
            {
                // Check valid type_info
                ea_t typeInfo = getEa(col + offsetof(_RTTICompleteObjectLocator, typeDescriptor));
                if (RTTI::type_info::isValid(typeInfo))
                {
                    ea_t classDescriptor = getEa(col + offsetof(_RTTICompleteObjectLocator, classDescriptor));
                    if (RTTI::_RTTIClassHierarchyDescriptor::isValid(classDescriptor))
                    {
                        //msg(EAFORMAT" " EAFORMAT " " EAFORMAT " \n", col, typeInfo, classDescriptor);
                        return(TRUE);
                    }
                }
            }
            else if (isDatabase64Bit && signature == 1)
			{
                // TODO: Can any of these be zero and still be valid?
                UINT objectLocator = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, objectBase));
                if (objectLocator != 0)
                {
                    UINT tdOffset = get_32bit(col + offsetof(_RTTICompleteObjectLocator, typeDescriptor));
                    if (tdOffset != 0)
                    {
                        UINT cdOffset = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, classDescriptor));
                        if (cdOffset != 0)
                        {
                            ea_t colBase = (col - (UINT64)objectLocator);

                            ea_t typeInfo = (colBase + (UINT64)tdOffset);
                            if (RTTI::type_info::isValid(typeInfo))
                            {
                                ea_t classDescriptor = (colBase + (UINT64) cdOffset);
                                if (RTTI::_RTTIClassHierarchyDescriptor::isValid(classDescriptor, colBase))
                                {
                                    //msg(EAFORMAT" " EAFORMAT " " EAFORMAT " \n", col, typeInfo, classDescriptor);
                                    return(TRUE);
                                }
                            }
                        }
                    }
                }
			}
		}
	}

	return(FALSE);
}

// Same as above but from an already validated type_info perspective
BOOL RTTI::_RTTICompleteObjectLocator::isValid2(ea_t col)
{
    // 'signature' should be zero
    UINT signature = -1;
    if (getVerify32((col + offsetof(_RTTICompleteObjectLocator, signature)), signature))
    {
        if (signature == 0)
        {
            // Verify CHD
            ea_t classDescriptor = getEa(col + offsetof(_RTTICompleteObjectLocator, classDescriptor));
            if (classDescriptor && (classDescriptor != BADADDR))
                return(RTTI::_RTTIClassHierarchyDescriptor::isValid(classDescriptor));
        }
    }

    return(FALSE);
}

// Place full COL hierarchy structures if they don't already exist
BOOL RTTI::_RTTICompleteObjectLocator::tryStruct(ea_t col)
{
	// If it doesn't have a name, IDA's analyzer missed it
	if (optionPlaceAtNamed || !hasName(col))
	{
		#if 0
		qstring buf;
		idaFlags2String(get_flags(col), buf);
		msg(EAFORMAT " fix COL (%s)\n", col, buf.c_str());
		#endif

		tryStructRTTI(col, s_CompleteObjectLocator_ID);

        if (!isDatabase64Bit)
        {
            // Put type_def
            ea_t typeInfo = getEa(col + offsetof(_RTTICompleteObjectLocator, typeDescriptor));
            type_info::tryStruct(typeInfo);

            // Place CHD hierarchy
            ea_t classDescriptor = getEa(col + offsetof(_RTTICompleteObjectLocator, classDescriptor));
            _RTTIClassHierarchyDescriptor::tryStruct(classDescriptor);
        }
        else
        {
            UINT tdOffset = get_32bit(col + offsetof(_RTTICompleteObjectLocator, typeDescriptor));
            UINT cdOffset = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, classDescriptor));
            UINT objectLocator = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, objectBase));
            ea_t colBase = (col - (UINT64)objectLocator);

            ea_t typeInfo = (colBase + (UINT64)tdOffset);
            type_info::tryStruct(typeInfo);

            ea_t classDescriptor = (colBase + (UINT64)cdOffset);
            _RTTIClassHierarchyDescriptor::tryStruct(classDescriptor, colBase);

            // Set absolute address comments
            ea_t ea = (col + offsetof(RTTI::_RTTICompleteObjectLocator, typeDescriptor));
            if (!hasComment(ea))
            {
                char buffer[64];
                sprintf_s(buffer, sizeof(buffer), "0x" EAFORMAT, typeInfo);
                setComment(ea, buffer, TRUE);
            }

            ea = (col + offsetof(RTTI::_RTTICompleteObjectLocator, classDescriptor));
            if (!hasComment(ea))
            {
                char buffer[64];
                sprintf_s(buffer, sizeof(buffer), "0x" EAFORMAT, classDescriptor);
                setComment(ea, buffer, TRUE);
            }
        }

		return TRUE;
	}

	return FALSE;
}


// --------------------------- Base Class Descriptor ---------------------------

// Return TRUE if address is a valid BCD
BOOL RTTI::_RTTIBaseClassDescriptor::isValid(ea_t bcd, ea_t colBase64)
{
    // TRUE if we've already seen it
    if (bcdSet.find(bcd) != bcdSet.end())
        return(TRUE);

    if (is_loaded(bcd))
    {
        // Check attributes flags first
        UINT attributes = -1;
        if (getVerify32((bcd + offsetof(_RTTIBaseClassDescriptor, attributes)), attributes))
        {
            // Valid flags are the lower byte only
            if ((attributes & 0xFFFFFF00) == 0)
            {
                // Check for valid type_info
                if (!isDatabase64Bit)
                {
                    return(RTTI::type_info::isValid(getEa(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor))));
                }
                else
                {
                    UINT tdOffset = get_32bit(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor));
                    ea_t typeInfo = (colBase64 + (UINT64)tdOffset);
                    return(RTTI::type_info::isValid(typeInfo));
                }
            }
        }
    }

    return(FALSE);
}

// Put BCD structure at address
void RTTI::_RTTIBaseClassDescriptor::tryStruct(ea_t bcd, __out_bcount(MAXSTR) LPSTR baseClassName, ea_t colBase64)
{
    // Only place it once
    if (bcdSet.find(bcd) != bcdSet.end())
    {
        // Seen already, just return type name
        ea_t typeInfo;
        if (!isDatabase64Bit)
        {
            typeInfo = getEa(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor));
        }
        else
        {
            UINT tdOffset = get_32bit(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor));
            typeInfo = (colBase64 + (UINT64)tdOffset);
        }

        char buffer[MAXSTR];
        type_info::getName(typeInfo, buffer, SIZESTR(buffer));
        strcpy_s(baseClassName, sizeof(buffer), SKIP_TD_TAG(buffer));
        return;
    }
    else
        bcdSet.insert(bcd);

    if (is_loaded(bcd))
    {
        UINT attributes = get_32bit(bcd + offsetof(_RTTIBaseClassDescriptor, attributes));
        tryStructRTTI(bcd, s_BaseClassDescriptor_ID, NULL, ((attributes & BCD_HASPCHD) > 0));

        // Has appended CHD?
        if (attributes & BCD_HASPCHD)
        {
            // yes, process it
            ea_t chdOffset = (bcd + (offsetof(_RTTIBaseClassDescriptor, attributes) + sizeof(UINT)));
            ea_t chd;

            if (!isDatabase64Bit)
            {
                fixEa(chdOffset);
                chd = getEa(chdOffset);
            }
            else
            {
                fixDword(chdOffset);
                UINT chdOffset32 = get_32bit(chdOffset);
                chd = (colBase64 + (UINT64)chdOffset32);

				if (!hasComment(chdOffset))
				{
					char buffer[64];
					sprintf_s(buffer, sizeof(buffer), "0x" EAFORMAT, chd);
					setComment(chdOffset, buffer, TRUE);
				}
            }

            if (is_loaded(chd))
                _RTTIClassHierarchyDescriptor::tryStruct(chd, colBase64);
            else
                _ASSERT(FALSE);
        }

        // Place type_info struct
        ea_t typeInfo;
        if (!isDatabase64Bit)
        {
            typeInfo = getEa(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor));
        }
        else
        {
            UINT tdOffset = get_32bit(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor));
            typeInfo = (colBase64 + (UINT64)tdOffset);
        }
        type_info::tryStruct(typeInfo);

        // Get raw type/class name
        char buffer[MAXSTR];
        type_info::getName(typeInfo, buffer, SIZESTR(buffer));
        strcpy_s(baseClassName, sizeof(buffer), SKIP_TD_TAG(buffer));

        if (!optionPlaceStructs && attributes)
        {
            // Place attributes comment
			ea_t ea = (bcd + offsetof(_RTTIBaseClassDescriptor, attributes));
			if (!hasComment(ea))
            {
                qstring s("");
                BOOL b = 0;
                #define ATRIBFLAG(_flag) { if (attributes & _flag) { if (b) s += " | ";  s += #_flag; b = 1; } }
                ATRIBFLAG(BCD_NOTVISIBLE);
                ATRIBFLAG(BCD_AMBIGUOUS);
                ATRIBFLAG(BCD_PRIVORPROTINCOMPOBJ);
                ATRIBFLAG(BCD_PRIVORPROTBASE);
                ATRIBFLAG(BCD_VBOFCONTOBJ);
                ATRIBFLAG(BCD_NONPOLYMORPHIC);
                ATRIBFLAG(BCD_HASPCHD);
                #undef ATRIBFLAG
                setComment(ea, s.c_str(), TRUE);
            }
        }

        // Give it a label
        if (!hasName(bcd))
        {
            // Name::`RTTI Base Class Descriptor at (0, -1, 0, 0)'
            ZeroMemory(buffer, sizeof(buffer));
            char buffer1[64] = { 0 }, buffer2[64] = { 0 }, buffer3[64] = { 0 }, buffer4[64] = { 0 };
            _snprintf_s(buffer, sizeof(buffer), SIZESTR(buffer), FORMAT_RTTI_BCD,
                mangleNumber(get_32bit(bcd + (offsetof(_RTTIBaseClassDescriptor, pmd) + offsetof(PMD, mdisp))), buffer1),
                mangleNumber(get_32bit(bcd + (offsetof(_RTTIBaseClassDescriptor, pmd) + offsetof(PMD, pdisp))), buffer2),
                mangleNumber(get_32bit(bcd + (offsetof(_RTTIBaseClassDescriptor, pmd) + offsetof(PMD, vdisp))), buffer3),
                mangleNumber(attributes, buffer4),
                baseClassName);

			setName(bcd, buffer);
        }
    }
    else
        _ASSERT(FALSE);
}


// --------------------------- Class Hierarchy Descriptor ---------------------------

// Return true if address is a valid CHD structure
BOOL RTTI::_RTTIClassHierarchyDescriptor::isValid(ea_t chd, ea_t colBase64)
{
    // TRUE if we've already seen it
    if (chdSet.find(chd) != chdSet.end())
        return(TRUE);

    if (is_loaded(chd))
    {
        // signature should be zero statically
        UINT signature = -1;
        if (getVerify32((chd + offsetof(_RTTIClassHierarchyDescriptor, signature)), signature))
        {
            if (signature == 0)
            {
                // Check attributes flags
                UINT attributes = -1;
                if (getVerify32((chd + offsetof(_RTTIClassHierarchyDescriptor, attributes)), attributes))
                {
                    // Valid flags are the lower nibble only
                    if ((attributes & 0xFFFFFFF0) == 0)
                    {
                        // Should have at least one base class
                        UINT numBaseClasses = 0;
                        if (getVerify32((chd + offsetof(_RTTIClassHierarchyDescriptor, numBaseClasses)), numBaseClasses))
                        {
                            if (numBaseClasses >= 1)
                            {
                                // Check the first BCD entry
                                ea_t baseClassArray;
                                if (!isDatabase64Bit)
                                {
                                    baseClassArray = getEa(chd + offsetof(_RTTIClassHierarchyDescriptor, baseClassArray));
                                }
                                else
                                {
                                    UINT baseClassArrayOffset = get_32bit(chd + offsetof(_RTTIClassHierarchyDescriptor, baseClassArray));
                                    baseClassArray = (colBase64 + (UINT64)baseClassArrayOffset);
                                }

                                if (is_loaded(baseClassArray))
                                {
                                    if (!isDatabase64Bit)
                                    {
                                        ea_t baseClassDescriptor = getEa(baseClassArray);
                                        return(RTTI::_RTTIBaseClassDescriptor::isValid(baseClassDescriptor));
                                    }
                                    else
                                    {
                                        ea_t baseClassDescriptor = (colBase64 + (UINT64)get_32bit(baseClassArray));
                                        return(RTTI::_RTTIBaseClassDescriptor::isValid(baseClassDescriptor, colBase64));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return(FALSE);
}


// Put CHD structure at address
void RTTI::_RTTIClassHierarchyDescriptor::tryStruct(ea_t chd, ea_t colBase64)
{
    // Only place it once per address
    if (chdSet.find(chd) != chdSet.end())
        return;
    else
        chdSet.insert(chd);

    if (is_loaded(chd))
    {
        // Place CHD
        tryStructRTTI(chd, s_ClassHierarchyDescriptor_ID);

        // Place attributes comment
        UINT attributes = get_32bit(chd + offsetof(_RTTIClassHierarchyDescriptor, attributes));
        if (!optionPlaceStructs && attributes)
        {
			ea_t ea = (chd + offsetof(_RTTIClassHierarchyDescriptor, attributes));
			if (!hasComment(ea))
            {
                qstring s("");
                BOOL b = 0;
                #define ATRIBFLAG(_flag) { if (attributes & _flag) { if (b) s += " | ";  s += #_flag; b = 1; } }
                ATRIBFLAG(CHD_MULTINH);
                ATRIBFLAG(CHD_VIRTINH);
                ATRIBFLAG(CHD_AMBIGUOUS);
                #undef ATRIBFLAG
                setComment(ea, s.c_str(), TRUE);
            }
        }

        // ---- Place BCD's ----
        UINT numBaseClasses = 0;
        if (getVerify32((chd + offsetof(_RTTIClassHierarchyDescriptor, numBaseClasses)), numBaseClasses))
        {
            // Get pointer
            ea_t baseClassArray;
            if (!isDatabase64Bit)
            {
                baseClassArray = getEa(chd + offsetof(_RTTIClassHierarchyDescriptor, baseClassArray));
            }
            else
            {
                UINT baseClassArrayOffset = get_32bit(chd + offsetof(_RTTIClassHierarchyDescriptor, baseClassArray));
                baseClassArray = (colBase64 + (UINT64)baseClassArrayOffset);

                ea_t ea = (chd + offsetof(RTTI::_RTTIClassHierarchyDescriptor, baseClassArray));
                if (!hasComment(ea))
                {
                    char buffer[MAXSTR];
                    _snprintf_s(buffer, sizeof(buffer), SIZESTR(buffer), "0x" EAFORMAT, baseClassArray);
                    setComment(ea, buffer, TRUE);
                }
            }

            if (baseClassArray && (baseClassArray != BADADDR))
            {
                // Create offset string based on input digits
                char format[128];
                if (!isDatabase64Bit)
                {
                    if (numBaseClasses > 1)
                    {
                        int digits = (int)strlen(_itoa(numBaseClasses, format, 10));
                        if (digits > 1)
                            _snprintf_s(format, sizeof(format), SIZESTR(format), "  BaseClass[%%0%dd]", digits);
                        else
                            strcpy_s(format, sizeof(format), "  BaseClass[%d]");
                    }
                }
                else
                {
                    if (numBaseClasses > 1)
                    {
                        int digits = (int)strlen(_itoa(numBaseClasses, format, 10));
                        if (digits > 1)
                            _snprintf_s(format, sizeof(format), SIZESTR(format), "  BaseClass[%%0%dd] 0x%%016I64X", digits);
                        else
                            strcpy_s(format, sizeof(format), "  BaseClass[%d] 0x%016I64X");
                    }
                }

                for (UINT i = 0; i < numBaseClasses; i++, baseClassArray += sizeof(UINT)) // sizeof(ea_t)
                {
                    char baseClassName[MAXSTR];
                    if (!isDatabase64Bit)
                    {
                        fixEa(baseClassArray);

                        // Add index comment to to it
                        if (!hasComment(baseClassArray))
                        {
                            if (numBaseClasses == 1)
                                setComment(baseClassArray, "  BaseClass", FALSE);
                            else
                            {
                                char ptrComent[MAXSTR];
                                _snprintf_s(ptrComent, sizeof(ptrComent), SIZESTR(ptrComent), format, i);
                                setComment(baseClassArray, ptrComent, false);
                            }
                        }

                        // Place BCD struct, and grab the base class name
                        _RTTIBaseClassDescriptor::tryStruct(getEa(baseClassArray), baseClassName);
                    }
                    else
                    {
                        fixDword(baseClassArray);
                        UINT bcOffset = get_32bit(baseClassArray);
                        ea_t bcd = (colBase64 + (UINT64)bcOffset);

                        // Add index comment to to it
                        if (!hasComment(baseClassArray))
                        {
                            if (numBaseClasses == 1)
                            {
                                char buffer[MAXSTR];
                                sprintf_s(buffer, sizeof(buffer), "  BaseClass 0x" EAFORMAT, bcd);
                                setComment(baseClassArray, buffer, FALSE);
                            }
                            else
                            {
                                char buffer[MAXSTR];
                                _snprintf_s(buffer, sizeof(buffer), SIZESTR(buffer), format, i, bcd);
                                setComment(baseClassArray, buffer, false);
                            }
                        }

                        // Place BCD struct, and grab the base class name
                        _RTTIBaseClassDescriptor::tryStruct(bcd, baseClassName, colBase64);
                    }

                    // Now we have the base class name, name and label some things
                    if (i == 0)
                    {
                        // Set array name
                        if (!hasName(baseClassArray))
                        {
                            // ??_R2A@@8 = A::`RTTI Base Class Array'
                            char mangledName[MAXSTR];
                            _snprintf_s(mangledName, sizeof(mangledName), SIZESTR(mangledName), FORMAT_RTTI_BCA, baseClassName);
							setName(baseClassArray, mangledName);
                        }

                        // Add a spacing comment line above us
                        if (!hasAnteriorComment(baseClassArray))
							setAnteriorComment(baseClassArray, "");

                        // Set CHD name
                        if (!hasName(chd))
                        {
                            // A::`RTTI Class Hierarchy Descriptor'
                            char mangledName[MAXSTR];
                            _snprintf_s(mangledName, sizeof(mangledName), SIZESTR(mangledName), FORMAT_RTTI_CHD, baseClassName);
							setName(chd, mangledName);
                        }
                    }
                }

                // Make following DWORD if it's bytes are zeros
                if (numBaseClasses > 0)
                {
                    if (is_loaded(baseClassArray))
                        if (get_32bit(baseClassArray) == 0)
                            fixDword(baseClassArray);
                }
            }
            else
                _ASSERT(FALSE);
        }
        else
            _ASSERT(FALSE);
    }
    else
        _ASSERT(FALSE);
}


// --------------------------- Vftable ---------------------------


// Get list of base class descriptor info
static void RTTI::getBCDInfo(ea_t col, __out bcdList &list, __out UINT &numBaseClasses)
{
	numBaseClasses = 0;

    ea_t chd;
    ea_t colBase;
    if (!isDatabase64Bit)
    {
        chd = getEa(col + offsetof(_RTTICompleteObjectLocator, classDescriptor));
    }
    else
    {
        UINT cdOffset = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, classDescriptor));
        UINT objectLocator = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, objectBase));
        colBase = (col - (UINT64)objectLocator);
        chd = (colBase + (UINT64)cdOffset);
    }

	if(chd)
	{
        if (numBaseClasses = get_32bit(chd + offsetof(_RTTIClassHierarchyDescriptor, numBaseClasses)))
		{
            list.resize(numBaseClasses);

			// Get pointer
            ea_t baseClassArray;
            if (!isDatabase64Bit)
            {
                baseClassArray = getEa(chd + offsetof(_RTTIClassHierarchyDescriptor, baseClassArray));
            }
            else
            {
                UINT bcaOffset = get_32bit(chd + offsetof(_RTTIClassHierarchyDescriptor, baseClassArray));
                baseClassArray = (colBase + (UINT64)bcaOffset);
            }

			if(baseClassArray && (baseClassArray != BADADDR))
			{
				for(UINT i = 0; i < numBaseClasses; i++, baseClassArray += sizeof(UINT)) // sizeof(ea_t)
				{
                    ea_t bcd;
                    ea_t typeInfo;
                    if (!isDatabase64Bit)
                    {
                        // Get next BCD
                        bcd = getEa(baseClassArray);

                        // Get type name
                        typeInfo = getEa(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor));
                    }
                    else
                    {
                        UINT bcdOffset = get_32bit(baseClassArray);
                        bcd = (colBase + (UINT64)bcdOffset);

                        UINT tdOffset = get_32bit(bcd + offsetof(_RTTIBaseClassDescriptor, typeDescriptor));
                        typeInfo = (colBase + (UINT64)tdOffset);
                    }
                    bcdInfo *bi = &list[i];
                    type_info::getName(typeInfo, bi->m_name, SIZESTR(bi->m_name));

					// Add info to list
                    UINT mdisp = get_32bit(bcd + (offsetof(_RTTIBaseClassDescriptor, pmd) + offsetof(PMD, mdisp)));
                    UINT pdisp = get_32bit(bcd + (offsetof(_RTTIBaseClassDescriptor, pmd) + offsetof(PMD, pdisp)));
                    UINT vdisp = get_32bit(bcd + (offsetof(_RTTIBaseClassDescriptor, pmd) + offsetof(PMD, vdisp)));
                    // As signed int
                    bi->m_pmd.mdisp = *((PINT) &mdisp);
                    bi->m_pmd.pdisp = *((PINT) &pdisp);
                    bi->m_pmd.vdisp = *((PINT) &vdisp);
                    bi->m_attribute = get_32bit(bcd + offsetof(_RTTIBaseClassDescriptor, attributes));

					//msg("   BN: [%d] \"%s\", ATB: %04X\n", i, szBuffer1, get_32bit((ea_t) &pBCD->attributes));
					//msg("       mdisp: %d, pdisp: %d, vdisp: %d, attributes: %04X\n", *((PINT) &mdisp), *((PINT) &pdisp), *((PINT) &vdisp), attributes);
				}
			}
		}
	}
}


// Process RTTI vftable info
// Returns TRUE if if vftable and wasn't named on entry
BOOL RTTI::processVftable(ea_t vft, ea_t col)
{
	BOOL result = FALSE;
    ea_t colBase;
    ea_t typeInfo;

    if (isDatabase64Bit)
    {
        UINT tdOffset = get_32bit(col + offsetof(_RTTICompleteObjectLocator, typeDescriptor));
        UINT objectLocator = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, objectBase));
        colBase = (col - (UINT64)objectLocator);
        typeInfo = (colBase + (UINT64)tdOffset);
    }

    // Verify and fix if vftable exists here
    vftable::vtinfo vi;
    if (vftable::getTableInfo(vft, vi))
    {
        //msg(EAFORMAT " - " EAFORMAT " c: %d\n", vi.start, vi.end, vi.methodCount);

	    // Get COL type name
        ea_t chd;
        if (!isDatabase64Bit)
        {
            typeInfo = getEa(col + offsetof(_RTTICompleteObjectLocator, typeDescriptor));
            chd = get_32bit(col + offsetof(_RTTICompleteObjectLocator, classDescriptor));
        }
        else
        {
            UINT cdOffset = get_32bit(col + offsetof(RTTI::_RTTICompleteObjectLocator, classDescriptor));
            chd = (colBase + (UINT64)cdOffset);
        }

        char colName[MAXSTR];
        type_info::getName(typeInfo, colName, SIZESTR(colName));
        char demangledColName[MAXSTR];
        getPlainTypeName(colName, demangledColName);

        UINT chdAttributes = get_32bit(chd + offsetof(_RTTIClassHierarchyDescriptor, attributes));
        UINT offset = get_32bit(col + offsetof(_RTTICompleteObjectLocator, offset));

	    // Parse BCD info
	    bcdList list;
        UINT numBaseClasses;
	    getBCDInfo(col, list, numBaseClasses);

        BOOL sucess = FALSE, isTopLevel = FALSE;
        qstring cmt;

	    // ======= Simple or no inheritance
        if ((offset == 0) && ((chdAttributes & (CHD_MULTINH | CHD_VIRTINH)) == 0))
	    {
		    // Set the vftable name
            if (!hasName(vft))
		    {
				result = TRUE;

                // Decorate raw name as a vftable. I.E. const Name::`vftable'
                char decorated[MAXSTR];
                _snprintf_s(decorated, sizeof(decorated), SIZESTR(decorated), FORMAT_RTTI_VFTABLE, SKIP_TD_TAG(colName));
                setName(vft, decorated);
		    }

		    // Set COL name. I.E. const Name::`RTTI Complete Object Locator'
            if (!hasName(col))
            {
                char decorated[MAXSTR];
                _snprintf_s(decorated, sizeof(decorated), SIZESTR(decorated), FORMAT_RTTI_COL, SKIP_TD_TAG(colName));
                setName(col, decorated);
            }

		    // Build object hierarchy string
            int placed = 0;
            if (numBaseClasses > 1)
            {
                // Parent
                char plainName[MAXSTR];
                getPlainTypeName(list[0].m_name, plainName);
                cmt.sprnt("%s%s: ", ((list[0].m_name[3] == 'V') ? "" : "struct "), plainName);
                placed++;
                isTopLevel = ((strcmp(list[0].m_name, colName) == 0) ? TRUE : FALSE);

                // Child object hierarchy
                for (UINT i = 1; i < numBaseClasses; i++)
                {
                    // Append name
                    getPlainTypeName(list[i].m_name, plainName);
                    cmt.cat_sprnt("%s%s, ", ((list[i].m_name[3] == 'V') ? "" : "struct "), plainName);
                    placed++;
                }

                // Nix the ending ',' for the last one
                if (placed > 1)
                    cmt.remove((cmt.length() - 2), 2);
            }
            else
            {
                // Plain, no inheritance object(s)
                cmt.sprnt("%s%s: ", ((colName[3] == 'V') ? "" : "struct "), demangledColName);
                isTopLevel = TRUE;
            }

            if (placed > 1)
                cmt += ';';

            sucess = TRUE;
	    }
	    // ======= Multiple inheritance, and, or, virtual inheritance hierarchies
        else
        {
            bcdInfo *bi = NULL;
            int index = 0;

            // Must be the top level object for the type
            if (offset == 0)
            {
                _ASSERT(strcmp(colName, list[0].m_name) == 0);
                bi = &list[0];
                isTopLevel = TRUE;
            }
            else
            {
                // Get our object BCD level by matching COL offset to displacement
                for (UINT i = 0; i < numBaseClasses; i++)
                {
                    if (list[i].m_pmd.mdisp == offset)
                    {
                        bi = &list[i];
                        index = i;
                        break;
                    }
                }

                // If not found in list, use the first base object instead
                if (!bi)
                {
                    //msg("** " EAFORMAT " MI COL class offset: %X(%d) not in BCD.\n", vft, offset, offset);
                    for (UINT i = 0; i < numBaseClasses; i++)
                    {
                        if (list[i].m_pmd.pdisp != -1)
                        {
                            bi = &list[i];
                            index = i;
                            break;
                        }
                    }
                }
            }

            if (bi)
            {
                // Top object level layout
                int placed = 0;
                if (isTopLevel)
                {
                    // Set the vft name
                    if (!hasName(vft))
                    {
						result = TRUE;

                        char decorated[MAXSTR];
                        _snprintf_s(decorated, sizeof(decorated), SIZESTR(decorated), FORMAT_RTTI_VFTABLE, SKIP_TD_TAG(colName));
                        setName(vft, decorated);
                    }

                    // COL name
                    if (!hasName(col))
                    {
                        char decorated[MAXSTR];
                        _snprintf_s(decorated, sizeof(decorated), SIZESTR(decorated), FORMAT_RTTI_COL, SKIP_TD_TAG(colName));
                        setName(col, decorated);
                    }

                    // Build hierarchy string starting with parent
                    char plainName[MAXSTR];
                    getPlainTypeName(list[0].m_name, plainName);
                    cmt.sprnt("%s%s: ", ((list[0].m_name[3] == 'V') ? "" : "struct "), plainName);
                    placed++;

                    // Concatenate forward child hierarchy
                    for (UINT i = 1; i < numBaseClasses; i++)
                    {
                        getPlainTypeName(list[i].m_name, plainName);
                        cmt.cat_sprnt("%s%s, ", ((list[i].m_name[3] == 'V') ? "" : "struct "), plainName);
                        placed++;
                    }
                    if (placed > 1)
                        cmt.remove((cmt.length() - 2), 2);
                }
                else
                {
                    // Combine COL and CHD name
                    char combinedName[MAXSTR];
                    _snprintf_s(combinedName, sizeof(combinedName), SIZESTR(combinedName), "%s6B%s@", SKIP_TD_TAG(colName), SKIP_TD_TAG(bi->m_name));

                    // Set vftable name
                    if (!hasName(vft))
                    {
						result = TRUE;

                        char decorated[MAXSTR];
						strcpy(decorated, FORMAT_RTTI_VFTABLE_PREFIX);
						strncat_s(decorated, MAXSTR, combinedName, (MAXSTR - (1 + SIZESTR(FORMAT_RTTI_VFTABLE_PREFIX))));
                        setName(vft, decorated);
                    }

                    // COL name
                    if (!hasName((ea_t) col))
                    {
						char decorated[MAXSTR];
						strcpy(decorated, FORMAT_RTTI_COL_PREFIX);
						strncat_s(decorated, MAXSTR, combinedName, (MAXSTR - (1 + SIZESTR(FORMAT_RTTI_COL_PREFIX))));
                        setName((ea_t) col, decorated);
                    }

                    // Build hierarchy string starting with parent
                    char plainName[MAXSTR];
                    getPlainTypeName(bi->m_name, plainName);
                    cmt.sprnt("%s%s: ", ((bi->m_name[3] == 'V') ? "" : "struct "), plainName);
                    placed++;

                    // Concatenate forward child hierarchy
                    if (++index < (int) numBaseClasses)
                    {
                        for (; index < (int) numBaseClasses; index++)
                        {
                            getPlainTypeName(list[index].m_name, plainName);
                            cmt.cat_sprnt("%s%s, ", ((list[index].m_name[3] == 'V') ? "" : "struct "), plainName);
                            placed++;
                        }
                        if (placed > 1)
                            cmt.remove((cmt.length() - 2), 2);
                    }

                    /*
                    Experiment, maybe better this way to show before and after to show it's location in the hierarchy
                    // Concatenate reverse child hierarchy
                    if (--index >= 0)
                    {
                        for (; index >= 0; index--)
                        {
                            getPlainTypeName(list[index].m_name, plainName);
                            cmt.cat_sprnt("%s%s, ", ((list[index].m_name[3] == 'V') ? "" : "struct "), plainName);
                            placed++;
                        }
                        if (placed > 1)
                            cmt.remove((cmt.length() - 2), 2);
                    }
                    */
                }

                if (placed > 1)
                    cmt += ';';

                sucess = TRUE;
            }
            else
                msg(EAFORMAT" ** Couldn't find a BCD for MI/VI hierarchy!\n", vft);
        }

        if (sucess)
        {
            // Store entry
            addTableEntry(((chdAttributes & 0xF) | ((isTopLevel == TRUE) ? RTTI::IS_TOP_LEVEL : 0)), vft, vi.methodCount, "%s@%s", demangledColName, cmt.c_str());

            // Add a separating comment above RTTI COL
			ea_t colPtr = (vft - EA_SIZE);
			fixEa(colPtr);
			//cmt.cat_sprnt("  %s O: %d, A: %d  (#classinformer)", attributeLabel(chdAttributes, numBaseClasses), offset, chdAttributes);
			cmt.cat_sprnt("  %s (#classinformer)", attributeLabel(chdAttributes));
			if (!hasAnteriorComment(colPtr))
				setAnteriorComment(colPtr, "\n; %s %s", ((colName[3] == 'V') ? "class" : "struct"), cmt.c_str());

            //vftable::processMembers(plainName, vft, end);
        }
    }
    else
	// Usually a typedef reference to a COL, not a vftable
    {
		#if 0
		qstring tmp;
		idaFlags2String(get_flags(vft), tmp);
        msg(EAFORMAT" ** Vftable attached to this COL, error? (%s)\n", vft, tmp.c_str());
		#endif

        // Just set COL name
        if (!hasName(col))
        {
            if (!isDatabase64Bit)
            {
                typeInfo = getEa(col + offsetof(_RTTICompleteObjectLocator, typeDescriptor));
            }
            char colName[MAXSTR];
            type_info::getName(typeInfo, colName, SIZESTR(colName));

            char decorated[MAXSTR];
            _snprintf_s(decorated, sizeof(decorated), SIZESTR(decorated), FORMAT_RTTI_COL, SKIP_TD_TAG(colName));
            setName(col, decorated);
        }
    }

	return result;
}
