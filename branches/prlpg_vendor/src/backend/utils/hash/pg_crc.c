/*-------------------------------------------------------------------------
 *
 * pg_crc.c
 *	  PostgreSQL CRC support
 *
 * See Ross Williams' excellent introduction
 * A PAINLESS GUIDE TO CRC ERROR DETECTION ALGORITHMS, available from
 * http://www.ross.net/crc/download/crc_v3.txt or several other net sites.
 *
 * We use a normal (not "reflected", in Williams' terms) CRC, using initial
 * all-ones register contents and a final bit inversion.
 *
 * The 64-bit variant is not used as of PostgreSQL 8.1, but we retain the
 * code for possible future use.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/hash/pg_crc.c,v 1.23 2010/01/07 04:53:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/* Use c.h so that this file can be built in either frontend or backend */
#include "c.h"


/*
 * This table is based on the polynomial
 *	x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1.
 * (This is the same polynomial used in Ethernet checksums, for instance.)
 */
const uint32 pg_crc32_table[256] = {
	0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
	0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
	0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
	0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
	0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
	0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
	0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
	0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
	0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
	0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
	0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
	0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
	0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
	0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
	0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
	0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
	0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
	0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
	0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
	0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
	0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
	0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
	0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
	0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
	0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
	0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
	0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
	0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
	0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
	0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
	0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
	0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
	0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
	0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
	0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
	0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
	0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
	0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
	0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
	0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
	0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
	0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
	0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
	0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
	0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
	0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
	0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
	0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
	0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
	0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
	0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
	0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
	0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
	0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
	0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
	0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
	0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
	0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
	0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
	0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
	0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
	0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
	0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
	0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};


#ifdef PROVIDE_64BIT_CRC

/*
 * This table is based on the polynomial
 *
 * x^64 + x^62 + x^57 + x^55 + x^54 + x^53 + x^52 + x^47 + x^46 + x^45 +
 * x^40 + x^39 + x^38 + x^37 + x^35 + x^33 + x^32 + x^31 + x^29 + x^27 +
 * x^24 + x^23 + x^22 + x^21 + x^19 + x^17 + x^13 + x^12 + x^10 + x^9 +
 * x^7 + x^4 + x + 1
 *
 * which is borrowed from the DLT1 spec
 * (ECMA-182, available from http://www.ecma.ch/ecma1/STAND/ECMA-182.HTM)
 */

#if SIZEOF_VOID_P < 8		/* this test must match the one in pg_crc.h */

const uint32 pg_crc64_table0[256] = {
	0x00000000, 0xA9EA3693,
	0x53D46D26, 0xFA3E5BB5,
	0x0E42ECDF, 0xA7A8DA4C,
	0x5D9681F9, 0xF47CB76A,
	0x1C85D9BE, 0xB56FEF2D,
	0x4F51B498, 0xE6BB820B,
	0x12C73561, 0xBB2D03F2,
	0x41135847, 0xE8F96ED4,
	0x90E185EF, 0x390BB37C,
	0xC335E8C9, 0x6ADFDE5A,
	0x9EA36930, 0x37495FA3,
	0xCD770416, 0x649D3285,
	0x8C645C51, 0x258E6AC2,
	0xDFB03177, 0x765A07E4,
	0x8226B08E, 0x2BCC861D,
	0xD1F2DDA8, 0x7818EB3B,
	0x21C30BDE, 0x88293D4D,
	0x721766F8, 0xDBFD506B,
	0x2F81E701, 0x866BD192,
	0x7C558A27, 0xD5BFBCB4,
	0x3D46D260, 0x94ACE4F3,
	0x6E92BF46, 0xC77889D5,
	0x33043EBF, 0x9AEE082C,
	0x60D05399, 0xC93A650A,
	0xB1228E31, 0x18C8B8A2,
	0xE2F6E317, 0x4B1CD584,
	0xBF6062EE, 0x168A547D,
	0xECB40FC8, 0x455E395B,
	0xADA7578F, 0x044D611C,
	0xFE733AA9, 0x57990C3A,
	0xA3E5BB50, 0x0A0F8DC3,
	0xF031D676, 0x59DBE0E5,
	0xEA6C212F, 0x438617BC,
	0xB9B84C09, 0x10527A9A,
	0xE42ECDF0, 0x4DC4FB63,
	0xB7FAA0D6, 0x1E109645,
	0xF6E9F891, 0x5F03CE02,
	0xA53D95B7, 0x0CD7A324,
	0xF8AB144E, 0x514122DD,
	0xAB7F7968, 0x02954FFB,
	0x7A8DA4C0, 0xD3679253,
	0x2959C9E6, 0x80B3FF75,
	0x74CF481F, 0xDD257E8C,
	0x271B2539, 0x8EF113AA,
	0x66087D7E, 0xCFE24BED,
	0x35DC1058, 0x9C3626CB,
	0x684A91A1, 0xC1A0A732,
	0x3B9EFC87, 0x9274CA14,
	0xCBAF2AF1, 0x62451C62,
	0x987B47D7, 0x31917144,
	0xC5EDC62E, 0x6C07F0BD,
	0x9639AB08, 0x3FD39D9B,
	0xD72AF34F, 0x7EC0C5DC,
	0x84FE9E69, 0x2D14A8FA,
	0xD9681F90, 0x70822903,
	0x8ABC72B6, 0x23564425,
	0x5B4EAF1E, 0xF2A4998D,
	0x089AC238, 0xA170F4AB,
	0x550C43C1, 0xFCE67552,
	0x06D82EE7, 0xAF321874,
	0x47CB76A0, 0xEE214033,
	0x141F1B86, 0xBDF52D15,
	0x49899A7F, 0xE063ACEC,
	0x1A5DF759, 0xB3B7C1CA,
	0x7D3274CD, 0xD4D8425E,
	0x2EE619EB, 0x870C2F78,
	0x73709812, 0xDA9AAE81,
	0x20A4F534, 0x894EC3A7,
	0x61B7AD73, 0xC85D9BE0,
	0x3263C055, 0x9B89F6C6,
	0x6FF541AC, 0xC61F773F,
	0x3C212C8A, 0x95CB1A19,
	0xEDD3F122, 0x4439C7B1,
	0xBE079C04, 0x17EDAA97,
	0xE3911DFD, 0x4A7B2B6E,
	0xB04570DB, 0x19AF4648,
	0xF156289C, 0x58BC1E0F,
	0xA28245BA, 0x0B687329,
	0xFF14C443, 0x56FEF2D0,
	0xACC0A965, 0x052A9FF6,
	0x5CF17F13, 0xF51B4980,
	0x0F251235, 0xA6CF24A6,
	0x52B393CC, 0xFB59A55F,
	0x0167FEEA, 0xA88DC879,
	0x4074A6AD, 0xE99E903E,
	0x13A0CB8B, 0xBA4AFD18,
	0x4E364A72, 0xE7DC7CE1,
	0x1DE22754, 0xB40811C7,
	0xCC10FAFC, 0x65FACC6F,
	0x9FC497DA, 0x362EA149,
	0xC2521623, 0x6BB820B0,
	0x91867B05, 0x386C4D96,
	0xD0952342, 0x797F15D1,
	0x83414E64, 0x2AAB78F7,
	0xDED7CF9D, 0x773DF90E,
	0x8D03A2BB, 0x24E99428,
	0x975E55E2, 0x3EB46371,
	0xC48A38C4, 0x6D600E57,
	0x991CB93D, 0x30F68FAE,
	0xCAC8D41B, 0x6322E288,
	0x8BDB8C5C, 0x2231BACF,
	0xD80FE17A, 0x71E5D7E9,
	0x85996083, 0x2C735610,
	0xD64D0DA5, 0x7FA73B36,
	0x07BFD00D, 0xAE55E69E,
	0x546BBD2B, 0xFD818BB8,
	0x09FD3CD2, 0xA0170A41,
	0x5A2951F4, 0xF3C36767,
	0x1B3A09B3, 0xB2D03F20,
	0x48EE6495, 0xE1045206,
	0x1578E56C, 0xBC92D3FF,
	0x46AC884A, 0xEF46BED9,
	0xB69D5E3C, 0x1F7768AF,
	0xE549331A, 0x4CA30589,
	0xB8DFB2E3, 0x11358470,
	0xEB0BDFC5, 0x42E1E956,
	0xAA188782, 0x03F2B111,
	0xF9CCEAA4, 0x5026DC37,
	0xA45A6B5D, 0x0DB05DCE,
	0xF78E067B, 0x5E6430E8,
	0x267CDBD3, 0x8F96ED40,
	0x75A8B6F5, 0xDC428066,
	0x283E370C, 0x81D4019F,
	0x7BEA5A2A, 0xD2006CB9,
	0x3AF9026D, 0x931334FE,
	0x692D6F4B, 0xC0C759D8,
	0x34BBEEB2, 0x9D51D821,
	0x676F8394, 0xCE85B507
};

const uint32 pg_crc64_table1[256] = {
	0x00000000, 0x42F0E1EB,
	0x85E1C3D7, 0xC711223C,
	0x49336645, 0x0BC387AE,
	0xCCD2A592, 0x8E224479,
	0x9266CC8A, 0xD0962D61,
	0x17870F5D, 0x5577EEB6,
	0xDB55AACF, 0x99A54B24,
	0x5EB46918, 0x1C4488F3,
	0x663D78FF, 0x24CD9914,
	0xE3DCBB28, 0xA12C5AC3,
	0x2F0E1EBA, 0x6DFEFF51,
	0xAAEFDD6D, 0xE81F3C86,
	0xF45BB475, 0xB6AB559E,
	0x71BA77A2, 0x334A9649,
	0xBD68D230, 0xFF9833DB,
	0x388911E7, 0x7A79F00C,
	0xCC7AF1FF, 0x8E8A1014,
	0x499B3228, 0x0B6BD3C3,
	0x854997BA, 0xC7B97651,
	0x00A8546D, 0x4258B586,
	0x5E1C3D75, 0x1CECDC9E,
	0xDBFDFEA2, 0x990D1F49,
	0x172F5B30, 0x55DFBADB,
	0x92CE98E7, 0xD03E790C,
	0xAA478900, 0xE8B768EB,
	0x2FA64AD7, 0x6D56AB3C,
	0xE374EF45, 0xA1840EAE,
	0x66952C92, 0x2465CD79,
	0x3821458A, 0x7AD1A461,
	0xBDC0865D, 0xFF3067B6,
	0x711223CF, 0x33E2C224,
	0xF4F3E018, 0xB60301F3,
	0xDA050215, 0x98F5E3FE,
	0x5FE4C1C2, 0x1D142029,
	0x93366450, 0xD1C685BB,
	0x16D7A787, 0x5427466C,
	0x4863CE9F, 0x0A932F74,
	0xCD820D48, 0x8F72ECA3,
	0x0150A8DA, 0x43A04931,
	0x84B16B0D, 0xC6418AE6,
	0xBC387AEA, 0xFEC89B01,
	0x39D9B93D, 0x7B2958D6,
	0xF50B1CAF, 0xB7FBFD44,
	0x70EADF78, 0x321A3E93,
	0x2E5EB660, 0x6CAE578B,
	0xABBF75B7, 0xE94F945C,
	0x676DD025, 0x259D31CE,
	0xE28C13F2, 0xA07CF219,
	0x167FF3EA, 0x548F1201,
	0x939E303D, 0xD16ED1D6,
	0x5F4C95AF, 0x1DBC7444,
	0xDAAD5678, 0x985DB793,
	0x84193F60, 0xC6E9DE8B,
	0x01F8FCB7, 0x43081D5C,
	0xCD2A5925, 0x8FDAB8CE,
	0x48CB9AF2, 0x0A3B7B19,
	0x70428B15, 0x32B26AFE,
	0xF5A348C2, 0xB753A929,
	0x3971ED50, 0x7B810CBB,
	0xBC902E87, 0xFE60CF6C,
	0xE224479F, 0xA0D4A674,
	0x67C58448, 0x253565A3,
	0xAB1721DA, 0xE9E7C031,
	0x2EF6E20D, 0x6C0603E6,
	0xF6FAE5C0, 0xB40A042B,
	0x731B2617, 0x31EBC7FC,
	0xBFC98385, 0xFD39626E,
	0x3A284052, 0x78D8A1B9,
	0x649C294A, 0x266CC8A1,
	0xE17DEA9D, 0xA38D0B76,
	0x2DAF4F0F, 0x6F5FAEE4,
	0xA84E8CD8, 0xEABE6D33,
	0x90C79D3F, 0xD2377CD4,
	0x15265EE8, 0x57D6BF03,
	0xD9F4FB7A, 0x9B041A91,
	0x5C1538AD, 0x1EE5D946,
	0x02A151B5, 0x4051B05E,
	0x87409262, 0xC5B07389,
	0x4B9237F0, 0x0962D61B,
	0xCE73F427, 0x8C8315CC,
	0x3A80143F, 0x7870F5D4,
	0xBF61D7E8, 0xFD913603,
	0x73B3727A, 0x31439391,
	0xF652B1AD, 0xB4A25046,
	0xA8E6D8B5, 0xEA16395E,
	0x2D071B62, 0x6FF7FA89,
	0xE1D5BEF0, 0xA3255F1B,
	0x64347D27, 0x26C49CCC,
	0x5CBD6CC0, 0x1E4D8D2B,
	0xD95CAF17, 0x9BAC4EFC,
	0x158E0A85, 0x577EEB6E,
	0x906FC952, 0xD29F28B9,
	0xCEDBA04A, 0x8C2B41A1,
	0x4B3A639D, 0x09CA8276,
	0x87E8C60F, 0xC51827E4,
	0x020905D8, 0x40F9E433,
	0x2CFFE7D5, 0x6E0F063E,
	0xA91E2402, 0xEBEEC5E9,
	0x65CC8190, 0x273C607B,
	0xE02D4247, 0xA2DDA3AC,
	0xBE992B5F, 0xFC69CAB4,
	0x3B78E888, 0x79880963,
	0xF7AA4D1A, 0xB55AACF1,
	0x724B8ECD, 0x30BB6F26,
	0x4AC29F2A, 0x08327EC1,
	0xCF235CFD, 0x8DD3BD16,
	0x03F1F96F, 0x41011884,
	0x86103AB8, 0xC4E0DB53,
	0xD8A453A0, 0x9A54B24B,
	0x5D459077, 0x1FB5719C,
	0x919735E5, 0xD367D40E,
	0x1476F632, 0x568617D9,
	0xE085162A, 0xA275F7C1,
	0x6564D5FD, 0x27943416,
	0xA9B6706F, 0xEB469184,
	0x2C57B3B8, 0x6EA75253,
	0x72E3DAA0, 0x30133B4B,
	0xF7021977, 0xB5F2F89C,
	0x3BD0BCE5, 0x79205D0E,
	0xBE317F32, 0xFCC19ED9,
	0x86B86ED5, 0xC4488F3E,
	0x0359AD02, 0x41A94CE9,
	0xCF8B0890, 0x8D7BE97B,
	0x4A6ACB47, 0x089A2AAC,
	0x14DEA25F, 0x562E43B4,
	0x913F6188, 0xD3CF8063,
	0x5DEDC41A, 0x1F1D25F1,
	0xD80C07CD, 0x9AFCE626
};

#else							/* use int64 implementation */

const uint64 pg_crc64_table[256] = {
	UINT64CONST(0x0000000000000000), UINT64CONST(0x42F0E1EBA9EA3693),
	UINT64CONST(0x85E1C3D753D46D26), UINT64CONST(0xC711223CFA3E5BB5),
	UINT64CONST(0x493366450E42ECDF), UINT64CONST(0x0BC387AEA7A8DA4C),
	UINT64CONST(0xCCD2A5925D9681F9), UINT64CONST(0x8E224479F47CB76A),
	UINT64CONST(0x9266CC8A1C85D9BE), UINT64CONST(0xD0962D61B56FEF2D),
	UINT64CONST(0x17870F5D4F51B498), UINT64CONST(0x5577EEB6E6BB820B),
	UINT64CONST(0xDB55AACF12C73561), UINT64CONST(0x99A54B24BB2D03F2),
	UINT64CONST(0x5EB4691841135847), UINT64CONST(0x1C4488F3E8F96ED4),
	UINT64CONST(0x663D78FF90E185EF), UINT64CONST(0x24CD9914390BB37C),
	UINT64CONST(0xE3DCBB28C335E8C9), UINT64CONST(0xA12C5AC36ADFDE5A),
	UINT64CONST(0x2F0E1EBA9EA36930), UINT64CONST(0x6DFEFF5137495FA3),
	UINT64CONST(0xAAEFDD6DCD770416), UINT64CONST(0xE81F3C86649D3285),
	UINT64CONST(0xF45BB4758C645C51), UINT64CONST(0xB6AB559E258E6AC2),
	UINT64CONST(0x71BA77A2DFB03177), UINT64CONST(0x334A9649765A07E4),
	UINT64CONST(0xBD68D2308226B08E), UINT64CONST(0xFF9833DB2BCC861D),
	UINT64CONST(0x388911E7D1F2DDA8), UINT64CONST(0x7A79F00C7818EB3B),
	UINT64CONST(0xCC7AF1FF21C30BDE), UINT64CONST(0x8E8A101488293D4D),
	UINT64CONST(0x499B3228721766F8), UINT64CONST(0x0B6BD3C3DBFD506B),
	UINT64CONST(0x854997BA2F81E701), UINT64CONST(0xC7B97651866BD192),
	UINT64CONST(0x00A8546D7C558A27), UINT64CONST(0x4258B586D5BFBCB4),
	UINT64CONST(0x5E1C3D753D46D260), UINT64CONST(0x1CECDC9E94ACE4F3),
	UINT64CONST(0xDBFDFEA26E92BF46), UINT64CONST(0x990D1F49C77889D5),
	UINT64CONST(0x172F5B3033043EBF), UINT64CONST(0x55DFBADB9AEE082C),
	UINT64CONST(0x92CE98E760D05399), UINT64CONST(0xD03E790CC93A650A),
	UINT64CONST(0xAA478900B1228E31), UINT64CONST(0xE8B768EB18C8B8A2),
	UINT64CONST(0x2FA64AD7E2F6E317), UINT64CONST(0x6D56AB3C4B1CD584),
	UINT64CONST(0xE374EF45BF6062EE), UINT64CONST(0xA1840EAE168A547D),
	UINT64CONST(0x66952C92ECB40FC8), UINT64CONST(0x2465CD79455E395B),
	UINT64CONST(0x3821458AADA7578F), UINT64CONST(0x7AD1A461044D611C),
	UINT64CONST(0xBDC0865DFE733AA9), UINT64CONST(0xFF3067B657990C3A),
	UINT64CONST(0x711223CFA3E5BB50), UINT64CONST(0x33E2C2240A0F8DC3),
	UINT64CONST(0xF4F3E018F031D676), UINT64CONST(0xB60301F359DBE0E5),
	UINT64CONST(0xDA050215EA6C212F), UINT64CONST(0x98F5E3FE438617BC),
	UINT64CONST(0x5FE4C1C2B9B84C09), UINT64CONST(0x1D14202910527A9A),
	UINT64CONST(0x93366450E42ECDF0), UINT64CONST(0xD1C685BB4DC4FB63),
	UINT64CONST(0x16D7A787B7FAA0D6), UINT64CONST(0x5427466C1E109645),
	UINT64CONST(0x4863CE9FF6E9F891), UINT64CONST(0x0A932F745F03CE02),
	UINT64CONST(0xCD820D48A53D95B7), UINT64CONST(0x8F72ECA30CD7A324),
	UINT64CONST(0x0150A8DAF8AB144E), UINT64CONST(0x43A04931514122DD),
	UINT64CONST(0x84B16B0DAB7F7968), UINT64CONST(0xC6418AE602954FFB),
	UINT64CONST(0xBC387AEA7A8DA4C0), UINT64CONST(0xFEC89B01D3679253),
	UINT64CONST(0x39D9B93D2959C9E6), UINT64CONST(0x7B2958D680B3FF75),
	UINT64CONST(0xF50B1CAF74CF481F), UINT64CONST(0xB7FBFD44DD257E8C),
	UINT64CONST(0x70EADF78271B2539), UINT64CONST(0x321A3E938EF113AA),
	UINT64CONST(0x2E5EB66066087D7E), UINT64CONST(0x6CAE578BCFE24BED),
	UINT64CONST(0xABBF75B735DC1058), UINT64CONST(0xE94F945C9C3626CB),
	UINT64CONST(0x676DD025684A91A1), UINT64CONST(0x259D31CEC1A0A732),
	UINT64CONST(0xE28C13F23B9EFC87), UINT64CONST(0xA07CF2199274CA14),
	UINT64CONST(0x167FF3EACBAF2AF1), UINT64CONST(0x548F120162451C62),
	UINT64CONST(0x939E303D987B47D7), UINT64CONST(0xD16ED1D631917144),
	UINT64CONST(0x5F4C95AFC5EDC62E), UINT64CONST(0x1DBC74446C07F0BD),
	UINT64CONST(0xDAAD56789639AB08), UINT64CONST(0x985DB7933FD39D9B),
	UINT64CONST(0x84193F60D72AF34F), UINT64CONST(0xC6E9DE8B7EC0C5DC),
	UINT64CONST(0x01F8FCB784FE9E69), UINT64CONST(0x43081D5C2D14A8FA),
	UINT64CONST(0xCD2A5925D9681F90), UINT64CONST(0x8FDAB8CE70822903),
	UINT64CONST(0x48CB9AF28ABC72B6), UINT64CONST(0x0A3B7B1923564425),
	UINT64CONST(0x70428B155B4EAF1E), UINT64CONST(0x32B26AFEF2A4998D),
	UINT64CONST(0xF5A348C2089AC238), UINT64CONST(0xB753A929A170F4AB),
	UINT64CONST(0x3971ED50550C43C1), UINT64CONST(0x7B810CBBFCE67552),
	UINT64CONST(0xBC902E8706D82EE7), UINT64CONST(0xFE60CF6CAF321874),
	UINT64CONST(0xE224479F47CB76A0), UINT64CONST(0xA0D4A674EE214033),
	UINT64CONST(0x67C58448141F1B86), UINT64CONST(0x253565A3BDF52D15),
	UINT64CONST(0xAB1721DA49899A7F), UINT64CONST(0xE9E7C031E063ACEC),
	UINT64CONST(0x2EF6E20D1A5DF759), UINT64CONST(0x6C0603E6B3B7C1CA),
	UINT64CONST(0xF6FAE5C07D3274CD), UINT64CONST(0xB40A042BD4D8425E),
	UINT64CONST(0x731B26172EE619EB), UINT64CONST(0x31EBC7FC870C2F78),
	UINT64CONST(0xBFC9838573709812), UINT64CONST(0xFD39626EDA9AAE81),
	UINT64CONST(0x3A28405220A4F534), UINT64CONST(0x78D8A1B9894EC3A7),
	UINT64CONST(0x649C294A61B7AD73), UINT64CONST(0x266CC8A1C85D9BE0),
	UINT64CONST(0xE17DEA9D3263C055), UINT64CONST(0xA38D0B769B89F6C6),
	UINT64CONST(0x2DAF4F0F6FF541AC), UINT64CONST(0x6F5FAEE4C61F773F),
	UINT64CONST(0xA84E8CD83C212C8A), UINT64CONST(0xEABE6D3395CB1A19),
	UINT64CONST(0x90C79D3FEDD3F122), UINT64CONST(0xD2377CD44439C7B1),
	UINT64CONST(0x15265EE8BE079C04), UINT64CONST(0x57D6BF0317EDAA97),
	UINT64CONST(0xD9F4FB7AE3911DFD), UINT64CONST(0x9B041A914A7B2B6E),
	UINT64CONST(0x5C1538ADB04570DB), UINT64CONST(0x1EE5D94619AF4648),
	UINT64CONST(0x02A151B5F156289C), UINT64CONST(0x4051B05E58BC1E0F),
	UINT64CONST(0x87409262A28245BA), UINT64CONST(0xC5B073890B687329),
	UINT64CONST(0x4B9237F0FF14C443), UINT64CONST(0x0962D61B56FEF2D0),
	UINT64CONST(0xCE73F427ACC0A965), UINT64CONST(0x8C8315CC052A9FF6),
	UINT64CONST(0x3A80143F5CF17F13), UINT64CONST(0x7870F5D4F51B4980),
	UINT64CONST(0xBF61D7E80F251235), UINT64CONST(0xFD913603A6CF24A6),
	UINT64CONST(0x73B3727A52B393CC), UINT64CONST(0x31439391FB59A55F),
	UINT64CONST(0xF652B1AD0167FEEA), UINT64CONST(0xB4A25046A88DC879),
	UINT64CONST(0xA8E6D8B54074A6AD), UINT64CONST(0xEA16395EE99E903E),
	UINT64CONST(0x2D071B6213A0CB8B), UINT64CONST(0x6FF7FA89BA4AFD18),
	UINT64CONST(0xE1D5BEF04E364A72), UINT64CONST(0xA3255F1BE7DC7CE1),
	UINT64CONST(0x64347D271DE22754), UINT64CONST(0x26C49CCCB40811C7),
	UINT64CONST(0x5CBD6CC0CC10FAFC), UINT64CONST(0x1E4D8D2B65FACC6F),
	UINT64CONST(0xD95CAF179FC497DA), UINT64CONST(0x9BAC4EFC362EA149),
	UINT64CONST(0x158E0A85C2521623), UINT64CONST(0x577EEB6E6BB820B0),
	UINT64CONST(0x906FC95291867B05), UINT64CONST(0xD29F28B9386C4D96),
	UINT64CONST(0xCEDBA04AD0952342), UINT64CONST(0x8C2B41A1797F15D1),
	UINT64CONST(0x4B3A639D83414E64), UINT64CONST(0x09CA82762AAB78F7),
	UINT64CONST(0x87E8C60FDED7CF9D), UINT64CONST(0xC51827E4773DF90E),
	UINT64CONST(0x020905D88D03A2BB), UINT64CONST(0x40F9E43324E99428),
	UINT64CONST(0x2CFFE7D5975E55E2), UINT64CONST(0x6E0F063E3EB46371),
	UINT64CONST(0xA91E2402C48A38C4), UINT64CONST(0xEBEEC5E96D600E57),
	UINT64CONST(0x65CC8190991CB93D), UINT64CONST(0x273C607B30F68FAE),
	UINT64CONST(0xE02D4247CAC8D41B), UINT64CONST(0xA2DDA3AC6322E288),
	UINT64CONST(0xBE992B5F8BDB8C5C), UINT64CONST(0xFC69CAB42231BACF),
	UINT64CONST(0x3B78E888D80FE17A), UINT64CONST(0x7988096371E5D7E9),
	UINT64CONST(0xF7AA4D1A85996083), UINT64CONST(0xB55AACF12C735610),
	UINT64CONST(0x724B8ECDD64D0DA5), UINT64CONST(0x30BB6F267FA73B36),
	UINT64CONST(0x4AC29F2A07BFD00D), UINT64CONST(0x08327EC1AE55E69E),
	UINT64CONST(0xCF235CFD546BBD2B), UINT64CONST(0x8DD3BD16FD818BB8),
	UINT64CONST(0x03F1F96F09FD3CD2), UINT64CONST(0x41011884A0170A41),
	UINT64CONST(0x86103AB85A2951F4), UINT64CONST(0xC4E0DB53F3C36767),
	UINT64CONST(0xD8A453A01B3A09B3), UINT64CONST(0x9A54B24BB2D03F20),
	UINT64CONST(0x5D45907748EE6495), UINT64CONST(0x1FB5719CE1045206),
	UINT64CONST(0x919735E51578E56C), UINT64CONST(0xD367D40EBC92D3FF),
	UINT64CONST(0x1476F63246AC884A), UINT64CONST(0x568617D9EF46BED9),
	UINT64CONST(0xE085162AB69D5E3C), UINT64CONST(0xA275F7C11F7768AF),
	UINT64CONST(0x6564D5FDE549331A), UINT64CONST(0x279434164CA30589),
	UINT64CONST(0xA9B6706FB8DFB2E3), UINT64CONST(0xEB46918411358470),
	UINT64CONST(0x2C57B3B8EB0BDFC5), UINT64CONST(0x6EA7525342E1E956),
	UINT64CONST(0x72E3DAA0AA188782), UINT64CONST(0x30133B4B03F2B111),
	UINT64CONST(0xF7021977F9CCEAA4), UINT64CONST(0xB5F2F89C5026DC37),
	UINT64CONST(0x3BD0BCE5A45A6B5D), UINT64CONST(0x79205D0E0DB05DCE),
	UINT64CONST(0xBE317F32F78E067B), UINT64CONST(0xFCC19ED95E6430E8),
	UINT64CONST(0x86B86ED5267CDBD3), UINT64CONST(0xC4488F3E8F96ED40),
	UINT64CONST(0x0359AD0275A8B6F5), UINT64CONST(0x41A94CE9DC428066),
	UINT64CONST(0xCF8B0890283E370C), UINT64CONST(0x8D7BE97B81D4019F),
	UINT64CONST(0x4A6ACB477BEA5A2A), UINT64CONST(0x089A2AACD2006CB9),
	UINT64CONST(0x14DEA25F3AF9026D), UINT64CONST(0x562E43B4931334FE),
	UINT64CONST(0x913F6188692D6F4B), UINT64CONST(0xD3CF8063C0C759D8),
	UINT64CONST(0x5DEDC41A34BBEEB2), UINT64CONST(0x1F1D25F19D51D821),
	UINT64CONST(0xD80C07CD676F8394), UINT64CONST(0x9AFCE626CE85B507)
};
#endif   /* SIZEOF_VOID_P < 8 */

#endif   /* PROVIDE_64BIT_CRC */
