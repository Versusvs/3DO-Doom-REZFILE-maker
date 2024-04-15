#include "stdafx.h"

#include <cstdlib>
#include <iostream>

#include "burger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

using namespace std;

typedef struct {
	Word Type;
	Word RezNum;
	LongWord Length;
	LongWord Offset;
} RezEntryInternal;

static Word LineNum;               /* Line being executed from the script */
static Word RezType;               /* Resource type */
static Word RezIDNum;              /* Resource ID Number */
static Word RezCount;              /* Number of resources loaded */
static Word RezFixed;				/* Fixed memory flag */
static char Delimiters[] = " \t\n";        /* Token delimiters */
static char NumDelimiters[] = " ,\t\n";    /* Value delimiters */
static char InputLine[256];        /* Input line from script */
static char RezFileName[32] = "REZFILE";
static char ScratchFileName[32] = "Scratch.TMP";
static FILE *tmpfp;                /* Temp data output file */
static LongWord TempFileLength;    /* Length of the data output */
//static Boolean SwapEndian;
typedef int Boolean;         /* True if I should swap the endian */
Boolean SwapEndian;
#define TRUE  1	
#define FALSE 0	

//LongWord SwapULong(LongWord val);
#define SwapULong(val) val = ( (((val) >> 24) & 0x000000FF) | (((val) >> 8) & 0x0000FF00) | (((val) <<  8) & 0x00FF0000) | (((val) << 24) & 0xFF000000) )
//#define SwapULong(val) (val << 24 | (val << 8 & 0xFF0000) | (val >> 8 & 0xFF00) | val >> 24 & 0xFF)

static Word LastRezNum;			/* Step value for resources */

#define CommandCount 6      /* Number of commands */
static char *Commands[] = {
	"TYPE","LOAD","ENDIAN","LOADNEXT","LOADFIXED","LOADHANDLE"
};

#define BUFFER_SIZE 0x40000UL
#define ENTRY_SIZE (0x2000*sizeof(RezEntryInternal))

static Byte *Buffer;         /* File buffer for data transfer */
static RezEntryInternal *EntArray;     /* Resource headers */
static MyRezHeader MyHeader;		/* Header to save to disk */

static Byte ErrorStr[80];


/**********************************

	Print out a script error

**********************************/

static void PrintError(char *Error)
{
	printf("# Error in Line %d, %s\n",LineNum,Error);
}

/**********************************

	Init the resource file and set up the defaults

**********************************/

static void InitMakeRez(void)
{
	RezType = 1;            /* Default type */
	RezIDNum = 1;           /* Default ID */
	RezCount = 0;           /* No resources loaded */
	RezFixed = 0;			/* Not fixed memory */
	TempFileLength = 0;     /* No data saved */
	tmpfp = fopen(ScratchFileName,"w+b");  /* Open up the temp data file */
}

/**********************************

	Remove an entry fron the entry list

**********************************/

static void RemoveEntry(RezEntryInternal *EntryPtr)
{
	RezEntryInternal *LastPtr;
	LongWord Length;

	--RezCount;			/* One less entry */
	LastPtr = &EntArray[RezCount];	/* Pointer to the final entry */
	Length = LastPtr-EntryPtr;		/* How many bytes to move? */
	if (Length) {			/* Shall I move them? */
		memmove(EntryPtr,EntryPtr+1,Length*sizeof(RezEntryInternal));
	}
}

/**********************************

	Scan the list of entries and find the lowest number

**********************************/

static RezEntryInternal *FindLowestType(void)
{
	RezEntryInternal *LowestPtr;
	RezEntryInternal *CurrentPtr;
	Word RezNum,Type;
	Word Count;

	CurrentPtr = EntArray;		/* Init the index */
	Count = RezCount;
	if (Count<2) {			/* No need to sort? */
		return CurrentPtr;	/* Just return this one */
	}
	RezNum = -1;		/* Use bogus numbers */
	Type = -1;
	do {
		if (CurrentPtr->Type<Type ||		/* Sort by type first */
			(CurrentPtr->Type == Type && CurrentPtr->RezNum<RezNum)) {
			LowestPtr = CurrentPtr;		/* Save as the lowest */
			RezNum = LowestPtr->RezNum;
			Type = LowestPtr->Type;
		}
		++CurrentPtr;
	} while (--Count);		/* All scanned? */
	return LowestPtr;
}

/**********************************

	Scan the list of entries and find a specific entry

**********************************/

static RezEntryInternal *FindEntry(Word RezNum,Word Type)
{
	RezEntryInternal *CurrentPtr;
	Word Count;

	CurrentPtr = EntArray;
	Count = RezCount;
	do {
		if (CurrentPtr->Type==Type && CurrentPtr->RezNum==RezNum) {
			return CurrentPtr;		/* Return the match */
		}
		++CurrentPtr;
	} while (--Count);
	return 0;
}

/**********************************

	Create runs of structs to sort all the types.
	Store the runs in Buffer and set MyHeader.Count and MemSize
	to reflect the output.

**********************************/

static void CompressHeaders(void)
{
	MyRezEntry *MainPtr;
	if (!RezCount) {		/* No resources at all? */
		MyHeader.MemSize = 0;		/* This is easy!! */
		MyHeader.Count = 0;
		return;
	}
	MainPtr = (MyRezEntry *)Buffer;
	do {		/* Perform while resources exist */
		Word Count;
		Word RezNum;
		Word Type;
		MyRezEntry2 *TempPtr;
		RezEntryInternal *EntryPtr;

		++MyHeader.Count;		/* Add one to the count */
		Count = 1;				/* There must be at least 1 */
		EntryPtr = FindLowestType();	/* Find the first entry */
		Type = EntryPtr->Type;
		RezNum = EntryPtr->RezNum;
		MainPtr->Type = Type;		/* Save the type and resource number */
		MainPtr->RezNum = RezNum;
		TempPtr = &MainPtr->Array[0];
		TempPtr->Offset = EntryPtr->Offset;	/* Initial offset and length */
		TempPtr->Length = EntryPtr->Length;
		TempPtr->MemPtr = 0;
		++TempPtr;
		RemoveEntry(EntryPtr);
		if (RezCount) {
			do {
				++RezNum;
				EntryPtr = FindEntry(RezNum,Type);
				if (!EntryPtr) {
					break;
				}
				TempPtr->Offset = EntryPtr->Offset;
				TempPtr->Length = EntryPtr->Length;
				TempPtr->MemPtr = 0;
				RemoveEntry(EntryPtr);
				++Count;
				++TempPtr;
			} while (RezCount);
		}
		MainPtr->Count = Count;
		MainPtr = (MyRezEntry *)TempPtr;
	} while (RezCount);
	MyHeader.MemSize = ((Byte *)MainPtr)-Buffer;
}

/**********************************

	Wrap up the resource file

**********************************/

static void WrapUpMakeRez(void)
{
	FILE *outfp;
	LongWord Length;
	Word i;
	Word Head;

	memcpy(MyHeader.Name,"BRGR",4);     /* Init the signature */
	CompressHeaders();			/* Compress the resource entry runs */

	i = MyHeader.Count;			/* Get the number of runs found */
	if (i) {					/* Any resources? */
		MyRezEntry *MainPtr;

		MainPtr = (MyRezEntry *)Buffer;	/* Index to the buffer */
		Length = MyHeader.MemSize+sizeof(MyHeader);	/* Offset to the end */
		do {
			MyRezEntry2 *TempPtr;
			Word j;

			j = MainPtr->Count;		/* Get the run count */
			if (SwapEndian) {		/* Swap bytes? */                            
				MainPtr->Count = SwapULong(MainPtr->Count);
				MainPtr->Type = SwapULong(MainPtr->Type);
				MainPtr->RezNum = SwapULong(MainPtr->RezNum);
			}
			TempPtr = &MainPtr->Array[0];	/* Index to the array */
			do {
				TempPtr->Offset+=Length;    /* Adjust to true offset */
				if (SwapEndian) {                                              
					TempPtr->Offset = SwapULong(TempPtr->Offset);
					TempPtr->Length = SwapULong(TempPtr->Length);
				}
				++TempPtr;		/* Next index */
			} while (--j);
			MainPtr = (MyRezEntry *)TempPtr;	/* Point to end of the table */
		} while (--i);         /* All done? */
	}

	Length = MyHeader.MemSize;		/* Get the memory length */
	
//	if (SwapEndian) {
//		MyHeader.Count = SwapULong(MyHeader.Count);	/* Swap the header */
//		MyHeader.MemSize = SwapULong(Length);
//	}
   
    if (SwapEndian) {                                                          
        MyHeader.Count = SwapULong(MyHeader.Count);	/* Swap the header */
        MyHeader.MemSize = SwapULong(Length);
    }
	
	outfp = fopen(RezFileName,"wb");    /* Open the final file */
	fwrite(&MyHeader,1,sizeof(MyHeader),outfp);     /* Save off the data header */
	printf("# Saving %s\n",&MyHeader);
#if 1
    if (SwapEndian) {
        MyHeader.MemSize = SwapULong(Length);
    }
	fwrite(Buffer,1,Length,outfp); 
                                     
//#if 0
	Length = TempFileLength;      /* How large is the output file? */
	fseek(tmpfp,0,SEEK_SET);       /* Reset to the beginning */
	if (Length) {		/* Any data to copy? */
		do {
			LongWord Chunk;
			if (Length>BUFFER_SIZE) {	/* Get the smaller, the length */
				Chunk = BUFFER_SIZE;		/* or the buffer size */
			} else {
				Chunk = Length;
			}
			fread(Buffer,1,Chunk,tmpfp);
			fwrite(Buffer,1,Chunk,outfp);
			Length -= Chunk;
		} while (Length);
	}
#endif
	fclose(tmpfp);              /* Close the temp data file */
	fclose(outfp);              /* Close the data file */
	remove(ScratchFileName);	/* Kill the scratch file */
}

/**********************************

	Set the Resource type

**********************************/

static void SetType(void)
{
	char *TextPtr;

	TextPtr = strtok(0,Delimiters);     /* Get the operand */
	if (!TextPtr) {
		PrintError("No operand for TYPE");  /* No operand! */
		return;
	}
	RezType = atoi(TextPtr);      /* Convert to value */
}

/**********************************

	Load in the resource file

**********************************/

static void LoadRezFile(Word Config)
{
	char *TextPtr;
	RezEntryInternal *RezPtr;
	LongWord Length,Chunk;
	FILE *fp;

	RezPtr = &EntArray[RezCount];
	RezPtr->Type = RezType;
	RezPtr->Offset = TempFileLength;
	if (RezFixed) {
		RezPtr->Offset |= 0x80000000;	/* Set the fixed flag */
	}

	if (Config!=1) {
		TextPtr = strtok(0,NumDelimiters);      /* Get the operand */
		if (!TextPtr) {
			PrintError("No operand for LOAD"); /* No operand! */
			return;
		}
		RezPtr->RezNum = atoi(TextPtr);      /* Convert to value */
		LastRezNum = RezPtr->RezNum;
	} else {
		++LastRezNum;
		RezPtr->RezNum = LastRezNum;
	}

	TextPtr = strtok(0,NumDelimiters);
	if (!TextPtr) {
		PrintError("Not enough parms for LOAD");
		return;
	}

	fp = fopen(TextPtr,"rb");
	if (!fp) {
		sprintf((char *)ErrorStr,"Can't open %s",TextPtr);
		PrintError((char *)ErrorStr);
		return;
	}
	fseek(fp,0,SEEK_END);
	Length = ftell(fp);      /* How large is the output file? */
	RezPtr->Length = Length;
	TempFileLength += Length;
	fseek(fp,0,SEEK_SET);       /* Reset to the beginning */
	if (Length) {
		do {
			if (Length>BUFFER_SIZE) {
				Chunk = BUFFER_SIZE;
			} else {
				Chunk = Length;
			}
			fread(Buffer,1,Chunk,fp);
			fwrite(Buffer,1,Chunk,tmpfp);
			Length -= Chunk;
		} while (Length);
	}
	fclose(fp);             /* Close the temp data file */
	++RezCount;             /* Valid file loaded in */
}

/**********************************

	What endian style do I write out my header info?

**********************************/

static void SetEndian(void)
{
	char *TextPtr;

	TextPtr = strtok(0,Delimiters);     /* Get the operand */
	if (!TextPtr) {
		PrintError("No operand for ENDIAN");  /* No operand! */
		return;
	}
	if (!strcmp(TextPtr,"BIG")) {   /* Big endian? */
	#ifdef __BIGENDIAN__
		SwapEndian = 0;
	#else
		SwapEndian = 1;
	#endif
		return;
	}
	if (!strcmp(TextPtr,"LITTLE")) {   /* Big endian? */
	#ifndef __BIGENDIAN__
		SwapEndian = 0;
	#else
		SwapEndian = 1;
	#endif
		return;
	}
	PrintError("Bad operand for ENDIAN");  /* No operand! */
}

/**********************************

	Execute the script tokens
	Input with the FILE* of the open script file
	I assume LineNum == 0.

**********************************/

static void MakeRez(FILE* fp)
{
	char *TextPtr;  /* Pointer to string token */
	Word i;         /* Index */

	InitMakeRez();    /* Init the output file and other variables */
	while (fgets(InputLine,sizeof(InputLine),fp)) { /* Get a string */
		++LineNum;          /* Adjust the line # */
		TextPtr = strtok(InputLine,Delimiters); /* Get the first token */
		if (!TextPtr) {
			continue;
		}
		i = 0;      /* Check for the first command */
		if (isalnum(TextPtr[0])) {  /* Comment? */
			do {
				if (!strcmp(TextPtr,Commands[i])) { /* Match? */
					switch (i) {        /* Execute the command */
					case 0:
						SetType();   /* Target machine */
						break;
					case 1:
						LoadRezFile(0);   /* Input art file type */
						break;
					case 2:
						SetEndian();    /* Swap endian if needed */
						break;
					case 3:
						LoadRezFile(1);	/* Load the next sequential record */
						break;
					case 4:
						RezFixed = 1;	/* Movable memory */
						break;
					case 5:
						RezFixed = 0;

					}
					break;      /* Don't parse anymore! */
				}
			} while (++i<CommandCount); /* Keep checking */
		}
		if (i==CommandCount) {      /* Didn't find it? */
			printf("# Command %s not implemented\n",TextPtr);
		}
	}
	WrapUpMakeRez();
}


/**********************************

	Main dispatcher for the converter

**********************************/

int _tmain(int argc, _TCHAR* argv[])
{
	FILE *fp;               /* Input file */
//cin<<argc;
argc=2;
//argv = ;
	if (argc!=2) {          /* Gotta have input and output arguments */
		printf("# Copyright 1995 by LogicWare\n"
			"# This program will create a resource data file using a script\n"
			"# Usage: MakeRez Infile\n");
		return 1;
	}
	Buffer = (Byte *)malloc(BUFFER_SIZE);
	if (!Buffer) {
		printf("# Not enough memory for buffer!\n");
		return 1;
	}
	EntArray = (RezEntryInternal *)malloc(ENTRY_SIZE);
	if (!EntArray) {
		free(Buffer);
		printf("# Not enough memory for resource entries!\n");
		return 1;
	}
//	fp = fopen(argv[1],"r");    /* Read the ASCII script */	
//    fp = fopen("BuildScr.txt","r");    /* Read the ASCII script */
    fp = fopen("BuildScr.txt","r");    /* Read the ASCII script */
	if (!fp) {
		printf("# Can't open script file %s.\n",argv[1]);    /* Oh oh */
		free(Buffer);
		free(EntArray);
		return 1;
	}
//	printf("# Opening BuildScr.txt %s\n",argv[1]);
	MakeRez(fp);     /* Slice it up! */
	fclose(fp);     /* Close the file */
	free(Buffer);
	free(EntArray);
//	return 0;
	
	cout << "Press the enter key to continue ...";
    cin.get();
    return 0;
}