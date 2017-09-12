/*
 *  faultinjector.h
 *  
 *
 *  Copyright 2009-2010, Greenplum Inc. All rights reserved.
 *
 */

#ifndef FAULTINJECTOR_H
#define FAULTINJECTOR_H

#include "pg_config_manual.h"

#define FAULTINJECTOR_MAX_SLOTS	16

/*
 * NB: This list needs to be kept in sync with:
 * - FaultInjectorTypeEnumToString
 * - the help message in clsInjectFault.py
 */
typedef enum FaultInjectorType_e {
	FaultInjectorTypeNotSpecified=0,
	
	FaultInjectorTypeSleep,
	
	FaultInjectorTypeFault,
	
	FaultInjectorTypeFatal,
	
	FaultInjectorTypePanic,
	
	FaultInjectorTypeError,
	
	FaultInjectorTypeInfiniteLoop,
	
	FaultInjectorTypeDataCorruption,
	
	FaultInjectorTypeSuspend,
	
	FaultInjectorTypeResume,
	
	FaultInjectorTypeSkip,
	
	FaultInjectorTypeMemoryFull,
	
	FaultInjectorTypeReset,
	
	FaultInjectorTypeStatus,
	
	FaultInjectorTypeSegv,
	
	FaultInjectorTypeInterrupt,

	FaultInjectorTypeFinishPending,

	FaultInjectorTypeCheckpointAndPanic,

	/* INSERT has to be done before that line */
	FaultInjectorTypeMax,
	
} FaultInjectorType_e;

/*
 *
 */
typedef enum DDLStatement_e {
	DDLNotSpecified=0,
	
	CreateDatabase,
	DropDatabase,
	
	CreateTable,
	DropTable,
	
	CreateIndex,
	AlterIndex,
	ReIndex,
	DropIndex,
	
	CreateFilespaces,
	DropFilespaces,
	
	CreateTablespaces,
	DropTablespaces,
	
	Truncate,
	
	Vacuum,
	
	/* INSERT has to be done before that line */
	DDLMax,
} DDLStatement_e;

/*
 *
 */
typedef enum FaultInjectorState_e {
	FaultInjectorStateNotInitialized=0,
		
	FaultInjectorStateWaiting,
		/* Request is waiting to be injected */
	
	FaultInjectorStateTriggered,
		/* Fault is injected */
	
	FaultInjectorStateCompleted,
		/* Fault was injected and completed successfully */
	
	FaultInjectorStateFailed,
		/* Fault was NOT injected */
	
} FaultInjectorState_e;


/*
 *
 */
typedef struct FaultInjectorEntry_s {
	
	char	 faultInjectorIdentifier[NAMEDATALEN];
	
	FaultInjectorType_e		faultInjectorType;
	
	int						sleepTime;
		/* in seconds, in use if fault injection type is sleep */
		
	DDLStatement_e			ddlStatement;
	
	char					databaseName[NAMEDATALEN];
	
	char					tableName[NAMEDATALEN];
	
	volatile	int			occurrence;
	volatile	 int	numTimesTriggered;
	volatile	FaultInjectorState_e	faultInjectorState;

		/* the state of the fault injection */
	char					bufOutput[2500];
	
} FaultInjectorEntry_s;


extern FaultInjectorType_e	FaultInjectorTypeStringToEnum(
									char*		faultTypeString);

extern DDLStatement_e FaultInjectorDDLStringToEnum(
									char*	ddlString);

extern Size FaultInjector_ShmemSize(void);

extern void FaultInjector_ShmemInit(void);

extern FaultInjectorType_e FaultInjector_InjectFaultIfSet(
							   char* identifier,
							   DDLStatement_e			 ddlStatement,
							   const char*				 databaseName,
							   const char*				 tableName);

extern int FaultInjector_SetFaultInjection(
							FaultInjectorEntry_s	*entry);


extern bool FaultInjector_IsFaultInjected(
							char* identifier);


#ifdef FAULT_INJECTOR
#define SIMPLE_FAULT_INJECTOR(FaultName) \
	FaultInjector_InjectFaultIfSet(FaultName, DDLNotSpecified, "", "")

#else
#define SIMPLE_FAULT_INJECTOR(FaultName)
#endif

#endif	/* FAULTINJECTOR_H */

