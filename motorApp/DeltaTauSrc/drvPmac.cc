/*
FILENAME...	drvPmac.cc
USAGE...	Driver level support for Delta Tau PMAC model.

Version:	$Revision: 1.1 $
Modified By:	$Author: sluiter $
Last Modified:	$Date: 2004-06-07 19:27:05 $
*/

/*
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *	      The Controls and Automation Group (AT-8)
 *	      Ground Test Accelerator
 *	      Accelerator Technology Division
 *	      Los Alamos National Laboratory
 *
 *      Co-developed with
 *	      The Controls and Computing Group
 *	      Accelerator Systems Division
 *	      Advanced Photon Source
 *	      Argonne National Laboratory
 *
 *
 * NOTES
 * -----
 * Verified with firmware:
 *
 * Modification Log:
 * -----------------
 * .00 04/17/04 rls - Copied from drvOms.cc
 */

#include	<vxLib.h>
#include	<sysLib.h>
#include	<string.h>
#include	<rebootLib.h>
#include	<logLib.h>
#include	<drvSup.h>
#include	<epicsVersion.h>
#if EPICS_MODIFICATION <= 4
extern "C" {
#include	<devLib.h>
}
#else
#include	<devLib.h>
#endif
#include	<dbAccess.h>
#include	<epicsThread.h>
#include	<epicsInterrupt.h>

#include	"motor.h"
#include	"drvPmac.h"
#include	"epicsExport.h"

#define CMD_CLEAR       '\030'	/* Control-X, clears command errors only */

#define	ALL_INFO	"QA RP RE EA"	/* jps: move QA to top. */
#define	AXIS_INFO	"QA RP"		/* jps: move QA to top. */
#define	ENCODER_QUERY	"EA"
#define	DONE_QUERY	"RA"

/* Control character responses. */
#define CMNDERR	0x03
#define ACK	0x06
#define CR	0x0D


/*----------------debugging-----------------*/
#ifdef	DEBUG
    volatile int drvPmacdebug = 0;
    #define Debug(l, f, args...) { if(l<=drvPmacdebug) printf(f,## args); }
#else
    #define Debug(l, f, args...)
#endif

/* Global data. */
int Pmac_num_cards = 0;

/* Local data required for every driver; see "motordrvComCode.h" */
#include	"motordrvComCode.h"

/* --- Local data common to all Pmac drivers. --- */
static char *Pmac_addrs = 0x0;	/* Base address of DPRAM. */
static char *Mbox_addrs = 0x0;	/* Base address of Mailbox. */
static epicsAddressType Pmac_ADDRS_TYPE;
static volatile unsigned PmacInterruptVector = 0;
static volatile epicsUInt8 PmacInterruptLevel = Pmac_INT_LEVEL;
static char Pmac_axis[] = {'1', '2', '3', '4', '5', '6', '7', '8'};
static double quantum;

/*----------------functions-----------------*/

/* Common local function declarations. */
static long report(int level);
static long init();
static void query_done(int, int, struct mess_node *);
static int set_status(int card, int signal);
RTN_STATUS send_mess(int card, char const *com, char c);
int recv_mess(int, char *, int);
static void motorIsr(int card);
static int motor_init();
static void Pmac_reset();

static RTN_STATUS PmacPut(int card, char *pcom);
static int motorIsrEnable(int card);
static void motorIsrDisable(int card);

struct driver_table Pmac_access =
{
    NULL,
    motor_send,
    motor_free,
    motor_card_info,
    motor_axis_info,
    &mess_queue,
    &queue_lock,
    &free_list,
    &freelist_lock,
    &motor_sem,
    &motor_state,
    &total_cards,
    &any_motor_in_motion,
    send_mess,
    recv_mess,
    set_status,
    query_done,
    NULL,
    &initialized,
    Pmac_axis
};

struct
{
    long number;
    long (*report) (int);
    long (*init) (void);
} drvPmac = {2, report, init};

epicsExportAddress(drvet, drvPmac);

static struct thread_args targs = {SCAN_RATE, &Pmac_access};

/*----------------functions-----------------*/

static long report(int level)
{
    int card;

    if (Pmac_num_cards <= 0)
	printf("    No VME8/44 controllers configured.\n");
    else
    {
	for (card = 0; card < Pmac_num_cards; card++)
	    if (motor_state[card])
		printf("    Pmac VME8/44 motor card %d @ 0x%X, id: %s \n", card,
		       (uint_t) motor_state[card]->localaddr,
		       motor_state[card]->ident);
    }
    return (0);
}

static long init()
{
    initialized = true;	/* Indicate that driver is initialized. */
    (void) motor_init();
    return ((long) 0);
}


static void query_done(int card, int axis, struct mess_node *nodeptr)
{
    char buffer[40];

    send_mess(card, DONE_QUERY, Pmac_axis[axis]);
    recv_mess(card, buffer, 1);

    if (nodeptr->status.Bits.RA_PROBLEM)
	send_mess(card, AXIS_STOP, Pmac_axis[axis]);
}


static int set_status(int card, int signal)
{
    struct mess_node *nodeptr;
    MOTOR_STATUS motorstat;
    struct mess_info *motor_info;
    /* Message parsing variables */
    char buff[BUFF_SIZE], outbuf[20];
    int rtn_state;
    double motorData;
    bool plusdir, ls_active = false, plusLS, minusLS;
    msta_field status;

    motor_info = &(motor_state[card]->motor_info[signal]);
    nodeptr = motor_info->motor_motion;
    status.All = motor_info->status.All;

    send_mess(card, "?", Pmac_axis[signal]);
    recv_mess(card, buff, 1);
    rtn_state = sscanf(buff, "%4hx%4hx%4hx", &motorstat.word1.All, &motorstat.word2.All,
		       &motorstat.word3.All);

    status.Bits.RA_DONE = (motorstat.word3.Bits.in_position == YES) ? 0 : 1;
    status.Bits.EA_POSITION = (motorstat.word1.Bits.amp_enabled == YES) ? 1 : 0;

    /* 
     * Parse motor position
     * Position string format: 1TP5.012,2TP1.123,3TP-100.567,...
     * Skip to substring for this motor, convert to double
     */

    sprintf(outbuf, "M%.2d61", (signal + 1));
    send_mess(card, outbuf, (char) NULL);
    recv_mess(card, buff, 1);

    motorData = atof(buff);

    if (motorData == motor_info->position)
    {
	if (nodeptr != 0)	/* Increment counter only if motor is moving. */
	    motor_info->no_motion_count++;
    }
    else
    {
	epicsInt32 newposition;

	newposition = NINT(motorData);
	status.Bits.RA_DIRECTION = (newposition >= motor_info->position) ? 1 : 0;
	motor_info->position = newposition;
	motor_info->no_motion_count = 0;
    }

    plusdir = (status.Bits.RA_DIRECTION) ? true : false;
    plusLS  = motorstat.word1.Bits.pos_limit_set;
    minusLS = motorstat.word1.Bits.neg_limit_set;

    /* Set limit switch error indicators. */
    if (plusLS == true)
    {
	status.Bits.RA_PLUS_LS = 1;
	if (plusdir == true)
	    ls_active = true;
    }
    else
	status.Bits.RA_PLUS_LS = 0;

    if (minusLS == true)
    {
	status.Bits.RA_MINUS_LS = 1;
	if (plusdir == false)
	    ls_active = true;
    }
    else
	status.Bits.RA_MINUS_LS = 0;

    /* encoder status */
    status.Bits.EA_SLIP	      = 0;
    status.Bits.EA_SLIP_STALL = 0;
    status.Bits.EA_HOME	      = 0;

    send_mess(card, "P", (char) NULL);
    recv_mess(card, buff, 1);
    motorData = atof(buff);
    motorData *= 32.0;	/* "P" command units are in 1/32 of a count. */
    motor_info->encoder_position = (int32_t) motorData;

    status.Bits.RA_PROBLEM	= 0;

    /* Parse motor velocity? */
    /* NEEDS WORK */

    motor_info->velocity = 0;

    if (!status.Bits.RA_DIRECTION)
	motor_info->velocity *= -1;

    rtn_state = (!motor_info->no_motion_count || ls_active == true ||
		status.Bits.RA_DONE | status.Bits.RA_PROBLEM) ? 1 : 0;

    /* Test for post-move string. */
    if ((status.Bits.RA_DONE || ls_active == true) && nodeptr != 0 &&
	nodeptr->postmsgptr != 0)
    {
	strcpy(buff, nodeptr->postmsgptr);
	send_mess(card, buff, (char) NULL);
	nodeptr->postmsgptr = NULL;
    }

    motor_info->status.All = status.All;
    return(rtn_state);
}


/*****************************************************/
/* send a message to the Pmac board		     */
/*		send_mess()			     */
/*****************************************************/
RTN_STATUS send_mess(int card, char const *com, char inchar)
{
    char outbuf[MAX_MSG_SIZE];
    RTN_STATUS return_code;

    if (strlen(com) > MAX_MSG_SIZE)
    {
	logMsg((char *) "drvPmac.cc:send_mess(); message size violation.\n",
	       0, 0, 0, 0, 0, 0);
	return (ERROR);
    }

    /* Check that card exists */
    if (!motor_state[card])
    {
	logMsg((char *) "drvPmac.cc:send_mess() - invalid card #%d\n", card,
	       0, 0, 0, 0, 0);
	return (ERROR);
    }

    /* Flush receive buffer */
    recv_mess(card, (char *) NULL, -1);

    if (inchar == NULL)
	strcpy(outbuf, com);
    else
    {
	strcpy(outbuf, "#?");
	outbuf[1] = inchar;
	strcat(outbuf, com);
    }

    Debug(9, "send_mess: ready to send message.\n");

    return_code = PmacPut(card, outbuf);

    if (return_code == OK)
    {
	Debug(4, "sent message: (%s)\n", outbuf);
    }
    else
    {
	Debug(4, "unable to send message (%s)\n", outbuf);
	return (ERROR);
    }

    return (return_code);
}


/*
 * FUNCTION... recv_mess(int card, char *com, int amount)
 *
 * INPUT ARGUMENTS...
 *	card - controller card # (0,1,...).
 *	*com - caller's response buffer.
 *	amount	| -1 = flush controller's output buffer.
 *		| >= 1 = the # of command responses to retrieve into caller's
 *				response buffer.
 *
 * LOGIC...
 *  IF controller card does not exist.
 *	ERROR RETURN.
 *  ENDIF
 *  IF "amount" indicates buffer flush.
 *	WHILE characters left in input buffer.
 *	    Call PmacGet().
 *	ENDWHILE
 *  ENDIF
 *
 *  FOR each message requested (i.e. "amount").
 *	Initialize head and tail pointers.
 *	Initialize retry counter and state indicator.
 *	WHILE retry count not exhausted, AND, state indicator is NOT at END.
 *	    IF characters left in controller's input buffer.
 *		Process input character.
 *	    ELSE IF command error occured - call PmacError().
 *		ERROR RETURN.
 *	    ENDIF
 *	ENDWHILE
 *	IF retry count exhausted.
 *	    Terminate receive buffer.
 *	    ERROR RETURN.
 *	ENDIF
 *	Terminate command response.
 *  ENDFOR
 *
 *  IF commands processed.
 *	Terminate response buffer.
 *  ELSE
 *	Clear response buffer.
 *  ENDIF
 *  NORMAL RETURN.
 */

int recv_mess(int card, char *com, int amount)
{
    volatile struct controller *pmotorState;
    volatile struct pmac_dpram *pmotor;
    volatile REPLY_STATUS *stptr;
    int trys;
    char control;

    pmotorState = motor_state[card];
    pmotor = (struct pmac_dpram *) pmotorState->localaddr;
    stptr = &pmotor->reply_status;
    
    /* Check that card exists */
    if (card >= total_cards)
    {
	Debug(1, "recv_mess - invalid card #%d\n", card);
	return (-1);
    }

    if (amount == -1)
    {
	bool timeout = false, flushed = false;
	control = stptr->Bits.cntrl_char;

	while (timeout == false && flushed == false)
	{
	    const double flush_delay = quantum;

	    if (control == NULL)
	    {
		Debug(6, "recv_mess() - flush wait on NULL\n");
		epicsThreadSleep(flush_delay);
		control = stptr->Bits.cntrl_char;
		if (control == NULL)
		    flushed = true;
		else
		    Debug(6, "recv_mess() - NULL -> %c\n", control);
	    }
	    else if (control == ACK)
	    {
		stptr->All = 0;
		Debug(6, "recv_mess() - flush wait on ACK\n");
		epicsThreadSleep(flush_delay);
		control = stptr->Bits.cntrl_char;
	    }
	    else if (control == CR)
	    {
		stptr->All = 0;
		Debug(6, "recv_mess() - flush wait on CR\n");
		for (trys = 0; trys < 10 && stptr->Bits.cntrl_char == NULL; trys++)
		{
		    epicsThreadSleep(quantum * trys);
		    Debug(6, "recv_mess() - flush wait #%d\n", trys);
		}
		if (trys >= 10)
		    timeout = true;
		control = stptr->Bits.cntrl_char;
	    }
	    else
	    {
		stptr->All = 0;
		errlogPrintf("%s(%d): ERROR = 0x%X\n", __FILE__, __LINE__, (unsigned int) control);
		epicsThreadSleep(flush_delay);
		control = stptr->Bits.cntrl_char;
	    }
	}

	if (timeout == true)
	    errlogPrintf("%s(%d): flush timeout\n", __FILE__, __LINE__);

	return(0);
    }

    for (trys = 0; trys < 10;)
    {
	if (stptr->All == 0)
	{
	    trys++;
	    epicsThreadSleep(quantum * 2.0);
	}
	else
	    break;
    }
    
    if (trys >= 10)
    {
	Debug(1, "recv_mess() timeout.\n");
	return(-1);
    }

    control = stptr->Bits.cntrl_char;

    if (control == CMNDERR)
    {
	stptr->All = 0;
	Debug(1, "recv_mess(): command error.\n");
	return(-1);
    }
    else if (control == ACK)
    {
	Debug(4, "recv_mess(): control = ACK\n");
	stptr->All = 0;
	return(recv_mess(card, com, amount));
    }
    else if (control == CR)
    {
	strcpy(com, (char *) &pmotor->response[0]);
	stptr->All = 0;
	Debug(4, "recv_mess(): card %d, msg: (%s)\n", card, com);
	return(0);
    }
    else
    {
	stptr->All = 0;
	errlogPrintf("%s(%d): ERROR = 0x%X\n", __FILE__, __LINE__, (unsigned int) control);
	return(-1);
    }
}


/*****************************************************/
/* Send Message to Pmac                               */
/*		PmacPut()			     */
/*****************************************************/
static RTN_STATUS PmacPut(int card, char *pmess)
{
    volatile struct controller *pmotorState;
    volatile struct pmac_dpram *pmotor;
    volatile REPLY_STATUS *stptr;
    int itera;

    pmotorState = motor_state[card];
    pmotor = (struct pmac_dpram *) pmotorState->localaddr;
    stptr = &pmotor->reply_status;
    
    for(itera = 0; itera < 10; itera++)
    {
	if(pmotor->out_cntrl_wd == 0)
	    break;
	else
	    epicsThreadSleep(0.010);
    }

    if(itera >= 10)
	return(ERROR);
    else
    {
	strcpy((char *) &pmotor->cmndbuff[0], pmess);
	pmotor->out_cntrl_wd = 1;
    }
    
    /* Wait for response. */
    for (itera = 0; itera < 10 && stptr->Bits.cntrl_char == NULL; itera++)
    {
	epicsThreadSleep(quantum * itera);
	Debug(7, "PmacPut() - response wait #%d\n", itera);
    }

    if (itera >= 10)
    {
	errlogPrintf("%s(%d): response timeout.\n", __FILE__, __LINE__);
	return(ERROR);
    }

    return (OK);
}



/*****************************************************/
/* Interrupt service routine.                        */
/* motorIsr()		                     */
/*****************************************************/
static void motorIsr(int card)
{
}

static int motorIsrEnable(int card)
{
    long status;
    
    status = devConnectInterrupt(intVME, PmacInterruptVector + card,
// Tornado 2.0.2    (void (*)()) motorIsr, (void *) card);
		    (devLibVOIDFUNCPTR) motorIsr, (void *) card);// Tornado 2.2
    

    status = devEnableInterruptLevel(Pmac_INTERRUPT_TYPE,
				     PmacInterruptLevel);

    return (OK);
}

static void motorIsrDisable(int card)
{
    long status;

    status = devDisconnectInterrupt(intVME, PmacInterruptVector + card,
// Tornado 2.0.2    (void (*)()) motorIsr);
		    (devLibVOIDFUNCPTR) motorIsr);// Tornado 2.2
    if (!RTN_SUCCESS(status))
	errPrintf(status, __FILE__, __LINE__, "Can't disconnect vector %d\n",
		  PmacInterruptVector + card);

}


/*****************************************************/
/* Configuration function for  module_types data     */
/* areas. PmacSetup()                                */
/*****************************************************/
int PmacSetup(int num_cards,	/* maximum number of cards in rack */
	     void *mbox,	/* Mailbox base address. */
	     void *addrs,	/* DPRAM Base Address */
	     int addrs_type,	/* VME address type; 24 - A24 or 32 - A32. */
	     unsigned vector,	/* noninterrupting(0), valid vectors(64-255) */
	     int int_level,	/* interrupt level (1-6) */
	     int scan_rate)	/* polling rate - in HZ */
{
    void *erraddr = 0;
    long status;

    if (num_cards < 1 || num_cards > Pmac_NUM_CARDS)
	Pmac_num_cards = Pmac_NUM_CARDS;
    else
	Pmac_num_cards = num_cards;

    switch(addrs_type)
    {
	case 24:
	    Pmac_ADDRS_TYPE = atVMEA24;

	    if ((uint32_t) mbox & 0xF)
		erraddr = mbox;
	    else if ((uint32_t) addrs & 0xF)
		erraddr = addrs;

	    if (erraddr != 0)
		Debug(1, "PmacSetup: invalid A24 address 0x%X\n", (uint_t) mbox);

	    break;
	case 32:
	    Pmac_ADDRS_TYPE = atVMEA32;
	    break;
	default:
	    Debug(1, "PmacSetup: invalid Address Type %d\n", (uint_t) addrs);
	    break;
    }

    status = devNoResponseProbe(Pmac_ADDRS_TYPE, (unsigned int) mbox, 1);

    if (status == 0)
    {
	Pmac_addrs = (char *) addrs;
	Mbox_addrs = (char *) mbox;
	*(Mbox_addrs + 0x121) = (char) ((unsigned long) Pmac_addrs >> 14); /* Select VME A19-A14 for DPRAM. */
    }
    else
    {
	errlogPrintf("%s(%d): Mailbox bus error - 0x%X\n", __FILE__, __LINE__,
		     (unsigned int) mbox);
	Pmac_num_cards = 0;
    }

    PmacInterruptVector = vector;
    if (vector < 64 || vector > 255)
    {
	if (vector != 0)
	{
	    Debug(1, "PmacSetup: invalid interrupt vector %d\n", vector);
	    PmacInterruptVector = (unsigned) Pmac_INT_VECTOR;
	}
    }

    if (int_level < 1 || int_level > 6)
    {
	Debug(1, "PmacSetup: invalid interrupt level %d\n", int_level);
	PmacInterruptLevel = Pmac_INT_LEVEL;
    }
    else
	PmacInterruptLevel = int_level;

    /* Set motor polling task rate */
    if (scan_rate >= 1 && scan_rate <= MAX_SCAN_RATE)
	targs.motor_scan_rate = scan_rate;
    else
    {
	targs.motor_scan_rate = SCAN_RATE;
	errlogPrintf("%s(%d): invalid poll rate - %d HZ\n", __FILE__, __LINE__,
		      scan_rate);
    }
    return(0);
}

/*****************************************************/
/* initialize all software and hardware		     */
/*		motor_init()			     */
/*****************************************************/
static int motor_init()
{
    volatile struct controller *pmotorState;
    volatile struct pmac_dpram *pmotor;
    long status;
    int card_index, motor_index;
    char axis_pos[50], encoder_pos[50];
    char *tok_save;
    int total_encoders = 0, total_axis = 0;
    volatile void *localaddr;
    void *probeAddr;
    bool errind;

    tok_save = NULL;
    quantum = epicsThreadSleepQuantum();
    Debug(5, "motor_init: epicsThreadSleepQuantum = %f\n", quantum);

    /* Check for setup */
    if (Pmac_num_cards <= 0)
    {
	Debug(1, "motor_init: *Pmac driver disabled* \n PmacSetup() is missing from startup script.\n");
	return (ERROR);
    }

    /* allocate space for total number of motors */
    motor_state = (struct controller **) malloc(Pmac_num_cards *
						sizeof(struct controller *));

    /* allocate structure space for each motor present */

    total_cards = Pmac_num_cards;

    if (rebootHookAdd((FUNCPTR) Pmac_reset) == ERROR)
	Debug(1, "vme8/44 motor_init: Pmac_reset disabled\n");

    for (card_index = 0; card_index < Pmac_num_cards; card_index++)
    {
	int8_t *startAddr;
	int8_t *endAddr;

	Debug(2, "motor_init: card %d\n", card_index);

	probeAddr = Pmac_addrs + (card_index * Pmac_BRD_SIZE);
	startAddr = (int8_t *) probeAddr + 1;
	endAddr = startAddr + Pmac_BRD_SIZE;

	Debug(9, "motor_init: devNoResponseProbe() on addr 0x%x\n", (uint_t) probeAddr);
	/* Scan memory space to assure card id */
	do
	{
	    status = devNoResponseProbe(Pmac_ADDRS_TYPE, (unsigned int) startAddr, 1);
	    startAddr += 0x100;
	} while (PROBE_SUCCESS(status) && startAddr < endAddr);

	if (PROBE_SUCCESS(status))
	{

	    status = devRegisterAddress(__FILE__, Pmac_ADDRS_TYPE,
					(size_t) probeAddr, Pmac_BRD_SIZE,
					(volatile void **) &localaddr);
	    Debug(9, "motor_init: devRegisterAddress() status = %d\n",
		  (int) status);
	    if (!RTN_SUCCESS(status))
	    {
		errPrintf(status, __FILE__, __LINE__,
			  "Can't register address 0x%x\n", (unsigned) probeAddr);
		return (ERROR);
	    }

	    Debug(9, "motor_init: localaddr = %x\n", (int) localaddr);

	    Debug(9, "motor_init: malloc'ing motor_state\n");
	    motor_state[card_index] = (struct controller *) malloc(sizeof(struct controller));
	    pmotorState = motor_state[card_index];
	    pmotorState->localaddr = (char *) localaddr;
	    pmotorState->motor_in_motion = 0;
	    pmotorState->cmnd_response = false;

	    /* Initialize DPRAM communication. */
	    pmotor = (struct pmac_dpram *) pmotorState->localaddr;
	    pmotor->out_cntrl_wd = 0;	/* Clear "Data ready from host" bit indicator. */
	    pmotor->out_cntrl_char = 0;	/* Clear "Buffer Control Character. */
	    pmotor->reply_status.All = 0;

	    send_mess(card_index, "TYPE", (char) NULL);
	    recv_mess(card_index, (char *) pmotorState->ident, 1);

	    send_mess(card_index, "VERSION", (char) NULL);
	    recv_mess(card_index, axis_pos, 1);
	    strcat((char *) &pmotorState->ident, ", ");
	    strcat((char *) &pmotorState->ident, axis_pos);

	    Debug(3, "Identification = %s\n", pmotorState->ident);

	    for (total_axis = 0, errind = false; errind == false &&
		 total_axis < Pmac_MAX_AXES; total_axis++)
	    {
		char outbuf[10];

		sprintf(outbuf, "I%.2d00", (total_axis + 1));
		send_mess(card_index, outbuf, (char) NULL);
		recv_mess(card_index, axis_pos, 1);
		if (strcmp(axis_pos, "0") == 0)
		    errind = true;
		else if (strcmp(axis_pos, "1") == 0)
		{
		    pmotorState->motor_info[total_axis].motor_motion = NULL;
		    pmotorState->motor_info[total_axis].status.All = 0;
		}
		else
		{
		    Debug(1, "Invalid response = \"%s\" to msg = \"%s\"\n", axis_pos, outbuf);
		}
	    }

	    pmotorState->total_axis = --total_axis;
	    Debug(3, "Total axis = %d\n", total_axis);

	    /*
	     * Enable interrupt-when-done if selected - driver depends on
	     * motor_state->total_axis  being set.
	     */
	    if (PmacInterruptVector)
	    {
		if (motorIsrEnable(card_index) == ERROR)
		    errPrintf(0, __FILE__, __LINE__, "Interrupts Disabled!\n");
	    }

	    for (total_encoders = 0, motor_index = 0; motor_index < total_axis; motor_index++)
	    {
		total_encoders++;
		pmotorState->motor_info[motor_index].encoder_present = YES;
	    }

	    for (motor_index = 0; motor_index < total_axis; motor_index++)
	    {
		pmotorState->motor_info[motor_index].status.All = 0;
		pmotorState->motor_info[motor_index].no_motion_count = 0;
		pmotorState->motor_info[motor_index].encoder_position = 0;
		pmotorState->motor_info[motor_index].position = 0;

		if (pmotorState->motor_info[motor_index].encoder_present == YES)
		    pmotorState->motor_info[motor_index].status.Bits.EA_PRESENT = 1;
		set_status(card_index, motor_index);
	    }

	    Debug(2, "Init Address=0x%8.8x\n", (uint_t) localaddr);
	    Debug(3, "Total encoders = %d\n\n", (int) total_encoders);
	}
	else
	{
	    Debug(3, "motor_init: Card NOT found!\n");
	    motor_state[card_index] = (struct controller *) NULL;
	}
    }

    any_motor_in_motion = 0;

    mess_queue.head = (struct mess_node *) NULL;
    mess_queue.tail = (struct mess_node *) NULL;

    free_list.head = (struct mess_node *) NULL;
    free_list.tail = (struct mess_node *) NULL;

    Debug(3, "Motors initialized\n");

    epicsThreadCreate((const char *) "Pmac_motor", 64, 5000, (EPICSTHREADFUNC) motor_task, (void *) &targs);

    Debug(3, "Started motor_task\n");
    return (0);
}

/* Disables interrupts. Called on CTL X reboot. */

static void Pmac_reset()
{
}

/*---------------------------------------------------------------------*/