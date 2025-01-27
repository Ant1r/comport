/* comport - PD external for unix/windows

 (c) 1998-2005  Winfried Ritsch (see LICENCE.txt)
 Institute for Electronic Music - Graz

 V 1.0
MP 20060603 memset and memcpy arguments were backwards for Windows version. close_serial doesn't crash now.
MP 20060618 make sure name is set up in comport_new (x->serial_device = test.serial_device) so help message works.
MP 20060619 implemented status outlet
MP 20060620 Add DTR and RTS control, add outputs to reflect CTS and DSR states.
MP 20060621 Do all the above for Windows too.
MP 20060709 All status goes out the status outlet when an info message is received
MP 20060824 added clock_delay call in comport_devicename
MP 20060924 added comport_enum to list available ports in Windows
MP 20060925 add devices message to enumerate actual devices, info just outputs current port state
MP 20061016 write_serial checks for GetOverlappedResult to avoid tx buffer overflow errors
MP 20070719 added "ports" method to output list of available ports on status outlet
MP 20071011 added comport_list and write_serials for list processing based on code by Thomas O Fredericks <tof@danslchamp.org>
MP 20071113 modified non-windows open_serial to set the index of the port when it's opened by name
MP 20080916 fixed Windows version stop bits to set and display 1, 1.5 or 2 for "stopbits" input of 1, 1.5 or 2
MP 20100201 use a buffer for writes, write takes place during clock callback comport_tick()

CR 20190514 disable input processing
JZ 20210321 purge error() in favour of pd_error()
JZ 20210321 cleanup t_comport struct
JZ 20210321 use (int) instead of t_float for members where applicable
JZ 20210321 allow the user to turn ON input processing (again)
JZ 20210321 allow the user to specify a device pattern as creation argument
*/

#include "m_pd.h"

#ifdef _MSC_VER
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#else
#include <sys/time.h>
#include <fcntl.h>
#include <sys/ioctl.h> /* for ioctl DTR */
#include <termios.h> /* for TERMIO ioctl calls */
#include <unistd.h>
#include <glob.h>
#define HANDLE int
#define INVALID_HANDLE_VALUE -1
#endif /* _WIN32 */

#include <string.h>
#include <errno.h>
#include <stdio.h>

#define comport_verbose if(x->x_verbose > 0)post


#define t_bool char

typedef struct comport
{
  /* basic object properties */
    t_object        x_obj;
    t_outlet        *x_data_outlet;
    t_outlet        *x_status_outlet;

#ifdef _WIN32
    HANDLE          comhandle; /* holds the comport handle */
    DCB             dcb; /* holds the comm pars */
    DCB             dcb_old; /* holds the comm pars */
    COMMTIMEOUTS    old_timeouts;
#else
    int             comhandle; /* holds the comport handle */
    struct termios  oldcom_termio; /* save the old com config */
    struct termios  com_termio; /* for the new com config */
#endif

  /* device specifications */
    t_symbol        *serial_device;
    const char*     pretty_name;
    char            serial_device_prefix[MAXPDSTRING];/* the device name without the number */
    short           comport; /* holds the comport # */

  /* device configuration */
    int             baud; /* holds the current baud rate */
    int             data_bits; /* holds the current number of data bits */
    int             parity_bit; /* holds the current parity */
    t_float         stop_bits; /* holds the current number of stop bits */
    t_bool          xonxoff; /* nonzero if xonxoff handshaking is on */
    t_bool          ctsrts; /* nonzero if ctsrts handshaking is on */
    t_bool          hupcl; /* nonzero if hang-up on close is on */

    int             rxerrors; /* holds the rx line errors */

    int             x_verbose; /* be more verbose */
    t_bool          x_inprocess; /* nonzero if we want to enable autoprocessing of input */

  /* buffers */
    unsigned char   *x_inbuf; /* read incoming serial to here */
    unsigned char   *x_outbuf; /* write outgoing serial from here */
    int             x_inbuf_len; /* length of inbuf */
    int             x_outbuf_len; /* length of outbuf */
    int             x_outbuf_wr_index; /* offset to next free location in x_outbuf */

  /* self-polling */
    t_clock         *x_clock;
    double          x_deltime;
    int             x_hit;
    int             x_retries;
    int             x_retry_count;
} t_comport;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#ifndef ON
#define ON 1
#define OFF 0
#endif

/* Serial Port Return Values */

#define NODATAAVAIL -1
#define RXERRORS -2
#define RXBUFOVERRUN -4
#define TXBUFOVERRUN -5

#define COMPORT_MAX 99
#define USE_DEVICENAME 9999 /* use the device name instead of the number */
#define COMPORT_BUF_SIZE 16384 /* this should be the largest possible packet size for a USB com port */

#ifdef _WIN32
/* we don't use the  table for msw, because we can set the number directly. */
/* This may result in more possible baud rates than the table contains. */
/*
static long baudspeedbittable[] =
{
    CBR_115200,
    CBR_57600,
    CBR_56000,
    CBR_38400,
    CBR_19200,
    CBR_14400,
    CBR_9600,
    CBR_4800,
    CBR_2400,
    CBR_1800,
    CBR_1200,
    CBR_600,
    CBR_300,
    CBR_110
};
*/
#else /* _WIN32 */

#ifdef  IRIX
#define OPENPARAMS (O_RDWR|O_NDELAY|O_NOCTTY)
#define TIONREAD FIONREAD         /* re map the IOCTL function */
#define BAUDRATE_230400 -1
#define BAUDRATE_115200 -1
#define BAUDRATE_57600  -1
#define BAUDRATE_38400  B38400
#else /* IRIX */
#define OPENPARAMS (O_RDWR|O_NDELAY|O_NOCTTY)
#define BAUDRATE_230400 B230400
#define BAUDRATE_115200 B115200
#define BAUDRATE_57600  B57600
#define BAUDRATE_38400  B38400
#endif /* else IRIX */

typedef struct baudbits_ {
  long rate;
  long speedbits;
} baudbits_t;

static
baudbits_t baudbitstable[] = {
#ifdef B4000000
  {4000000, B4000000},
#endif
#ifdef B3500000
  {3500000, B3500000},
#endif
#ifdef B3000000
  {3000000, B3000000},
#endif
#ifdef B2500000
  {2500000, B2500000},
#endif
#ifdef B2000000
  {2000000, B2000000},
#endif
#ifdef B1500000
  {1500000, B1500000},
#endif
#ifdef B1152000
  {1152000, B1152000},
#endif
#ifdef B1000000
  {1000000, B1000000},
#endif
#ifdef B921600
  {921600, B921600},
#endif
#ifdef B576000
  {576000, B576000},
#endif
#ifdef B500000
  {500000, B500000},
#endif
#ifdef B460800
  {460800, B460800},
#endif
#ifdef B230400
  {230400, B230400},
#else
  /* previously, this was supported without an #ifdef */
# warning baudrate 230400 not supported (anymore)?
#endif
#ifdef B115200
  {115200, B115200},
#endif
#ifdef B57600
  {57600, B57600},
#endif
#ifdef B38400
  {38400, B38400},
#endif
  {19200, B19200},
  {9600, B9600},
  {4800, B4800},
  {2400, B2400},
  {1800, B1800},
  {1200, B1200},
  {600, B600},
  {300, B300},
  {200, B200},
  {150, B150},
  {134, B134},
  {110, B110},
  {75, B75},
  {50, B50},
  {0, B0}
};



struct timeval null_tv;

#endif /* else _WIN32 */

/* From man cfsetospeed:
       cfsetospeed()  sets  the  output  baud  rate stored in the
       termios structure pointed to by termios_p to speed,  which
       must be one of these constants:
            B0
            B50
            B75
            B110
            B134
            B150
            B200
            B300
            B600
            B1200
            B1800
            B2400
            B4800
            B9600
            B19200
            B38400
            B57600
            B115200
            B230400
  The  zero  baud rate, B0, is used to terminate the connection.
  If B0 is specified, the modem control  lines  shall
  no longer be asserted.  Normally, this will disconnect the
  line.*/

t_class *comport_class;

static void comport_pollintervall(t_comport *x, t_floatarg g);
static void comport_retries(t_comport *x, t_floatarg g);
static void comport_tick(t_comport *x);
static int set_baudrate(t_comport *x, int baud);
static int set_bits(t_comport *x, int nr);
static int set_parity(t_comport *x, int n);
static float set_stopflag(t_comport *x, t_float nr);
static int set_ctsrts(t_comport *x, int nr);
static int set_dtr(t_comport *x, int nr);
static int set_break(t_comport *x, int on);
static int set_rts(t_comport *x, int nr);
static int set_xonxoff(t_comport *x, int nr);
static int set_serial(t_comport *x);
static int set_hupcl(t_comport *x, int nr);
static int write_serial(t_comport *x, unsigned char serial_byte);
static int write_serials(t_comport *x, unsigned char *serial_buf, int buf_length);
static int comport_get_dsr(t_comport *x);
static int comport_get_cts(t_comport *x);
#ifdef _WIN32
static HANDLE open_serial(unsigned int com_num, t_comport *x);
static HANDLE close_serial(t_comport *x);
#else
static int open_serial(unsigned int com_num, t_comport *x);
static int close_serial(t_comport *x);
static long get_baud_ratebits(t_comport*x, long *baud);
#endif
static void comport_pollintervall(t_comport *x, t_floatarg g);
static void comport_tick(t_comport *x);
static void comport_float(t_comport *x, t_float f);
static void comport_list(t_comport *x, t_symbol *s, int argc, t_atom *argv);
static void *comport_new(t_symbol *s, int argc, t_atom *argv);
static void comport_free(t_comport *x);
static void comport_baud(t_comport *x,t_floatarg f);
static void comport_bits(t_comport *x,t_floatarg f);
static void comport_parity(t_comport *x,t_floatarg f);
static void comport_stopbit(t_comport *x,t_floatarg f);
static void comport_rtscts(t_comport *x,t_floatarg f);
static void comport_dtr(t_comport *x,t_floatarg f);
static void comport_rts(t_comport *x,t_floatarg f);
static void comport_break(t_comport *x,t_floatarg f);
static void comport_xonxoff(t_comport *x,t_floatarg f);
static void comport_hupcl(t_comport *x,t_floatarg f);
static void comport_close(t_comport *x);
static void comport_open(t_comport *x, t_floatarg f);
static void comport_devicename(t_comport *x, t_symbol *s);
static void comport_print(t_comport *x, t_symbol *s, int argc, t_atom *argv);
static void comport_output_status(t_comport *x, t_symbol *selector, t_float output_value);
static void comport_output_port_status(t_comport *x);
static void comport_output_dsr_status(t_comport *x);
static void comport_output_cts_status(t_comport *x);
static void comport_output_baud_rate(t_comport *x);
static void comport_output_parity_bit(t_comport *x);
static void comport_output_stop_bits(t_comport *x);
static void comport_output_data_bits(t_comport *x);
static void comport_output_rtscts(t_comport *x);
static void comport_output_xonxoff(t_comport *x);
static void comport_output_hupcl(t_comport *x);
static void comport_output_rxerrors(t_comport *x);
static void comport_enum(t_comport *x);
static void comport_info(t_comport *x);
static void comport_devices(t_comport *x);
static void comport_ports(t_comport *x);
static void comport_set_verbose(t_comport *x, t_floatarg f);
static void comport_help(t_comport *x);
void comport_setup(void);

/* --------- sys independent serial setup helpers ---------------- */



/* ------------ sys dependent serial setup helpers ---------------- */


/* --------------------- NT ------------------------------------ */

#ifdef _WIN32

static int set_baudrate(t_comport *x, int baud)
{
    x->dcb.BaudRate = (DWORD)baud ;//!!!try directly setting any baud rate...was get_baud_ratebits(&baud);
    return baud;
}

/* bits are 5,6,7,8(default) */

static int set_bits(t_comport *x, int nr)
{
    if(nr < 4 && nr > 8) nr = 8;

    /*  number of bits/byte, 4-8  */
    return x->dcb.ByteSize = nr;
}


/* 1 ... Parity even, -1 parity odd , 0 (default) no parity */
static int set_parity(t_comport *x, int n)
{
    switch(n)
    {
        case 1:
            x->dcb.fParity = TRUE; /*  enable parity checking */
            x->dcb.Parity = 2;     /*  0-4=no,odd,even,mark,space  */
            return 1;
        case -1:
            x->dcb.fParity = TRUE; /*  enable parity checking */
            x->dcb.Parity = 1;     /*  0-4=no,odd,even,mark,space  */
            return -1;
        default:
            x->dcb.fParity = FALSE; /*  enable parity checking */
            x->dcb.Parity = 0;     /*  0-4=no,odd,even,mark,space  */
    }
    return 0;
}


/* Windows lets you have three different stop bit styles: 1,1.5 and 2 */
/* The message argument is the number of stop bits */
static float set_stopflag(t_comport *x, t_float nr)
{
    if(nr == 1)
    {
        x->dcb.StopBits = 0; /*  ONESTOPBIT = 0  */
        return nr;
    }
    if(nr == 2)
    {
        x->dcb.StopBits = 2; /*  TWOSTOPBITS = 2  */
        return nr;
    }
    if(nr == 1.5)
    {
        x->dcb.StopBits = 1; /*  ONE5STOPBITS = 1  */
        return nr;
    }
    pd_error(x, "comport stopbit number (%g) out of range (0, 1.5, 2)", nr);
    return 0;
}

/* never tested */
static int set_ctsrts(t_comport *x, int nr)
{
    if(nr == 1)
    {
        x->dcb.fOutxCtsFlow = TRUE; /*  CTS output flow control  */
        x->dcb.fRtsControl = RTS_CONTROL_ENABLE; /*  RTS flow control  */
        return 1;
    }
    x->dcb.fOutxCtsFlow = FALSE; /*  CTS output flow control  */
    x->dcb.fRtsControl = RTS_CONTROL_DISABLE; /*  RTS flow control  */
    return 0;
}

static int set_dtr(t_comport *x, int nr)
{
    HANDLE  fd = x->comhandle;
    BOOL    status;
    DWORD   dwFunc = (nr==0)?CLRDTR:SETDTR;

    if (fd == INVALID_HANDLE_VALUE) return -1;

    status = EscapeCommFunction(fd, dwFunc);
    if (status != 0) return nr;
    return -1; /* didn't work, GetLastError tells why */
}

static int set_rts(t_comport *x, int nr)
{
    HANDLE  fd = x->comhandle;
    BOOL    status;
    DWORD   dwFunc = (nr==0)?CLRRTS:SETRTS;

    if (fd == INVALID_HANDLE_VALUE) return -1;

    status = EscapeCommFunction(fd, dwFunc);
    if (status != 0) return nr;
    return -1; /* didn't work, GetLastError tells why */
}

static int set_break(t_comport *x, int on)
{
    HANDLE  fd = x->comhandle;
    BOOL    status;
    DWORD   dwFunc = (on==0)?CLRBREAK:SETBREAK;

    if (fd == INVALID_HANDLE_VALUE) return -1;

    status = EscapeCommFunction(fd, dwFunc);
    if (status != 0) return on;
    return -1; /* didn't work, GetLastError tells why */
}

static int set_xonxoff(t_comport *x, int nr)
{
    /*   x->dcb.fTXContinueOnXoff = FALSE;  XOFF continues Tx  */
    if(nr == 1)
    {
        x->dcb.fOutX  = TRUE; /*  XON/XOFF out flow control  */
        x->dcb.fInX  = TRUE; /*  XON/XOFF in flow control */
        return 1;
    }
    x->dcb.fOutX  = FALSE; /*  XON/XOFF out flow control  */
    x->dcb.fInX  = FALSE; /*  XON/XOFF in flow control */
    return 0;
}

static int set_serial(t_comport *x)
{
    if (SetCommState(x->comhandle, &(x->dcb))) return 1;
    /* Didn't work. Get the actual state of the device */
    GetCommState(x->comhandle, &(x->dcb));
    x->baud = x->dcb.BaudRate;
    x->data_bits = x->dcb.ByteSize;
    x->parity_bit = x->dcb.fParity;
/* ONESTOPBIT=0, ONE5STOPBITS=1, TWOSTOPBITS=2*/
    if(x->dcb.StopBits == 0) x->stop_bits = 1;
    else if(x->dcb.StopBits == 1) x->stop_bits = 1.5;
    else if(x->dcb.StopBits == 2) x->stop_bits = 2;
    x->xonxoff = (x->dcb.fOutX)?1:0;
    x->ctsrts = (x->dcb.fOutxCtsFlow)?1:0;
    return 0;
}

static int set_hupcl(t_comport *x, int nr)
{
    // this is a dummy function since Windows doesn't use HUPCL, AFAIK hans@eds.org
  (void)x;
  (void)nr;
  return 1;
}

static HANDLE open_serial(unsigned int com_num, t_comport *x)
{
    const char* pretty_name = 0;
    HANDLE          fd;
    COMMTIMEOUTS    timeouts;
    char            buffer[MAX_PATH];
    int            *baud = &(x->baud);
    DWORD           dw;
    int             i;
    char            *errStr;

    if (com_num != USE_DEVICENAME)
    {
        if(com_num < 1 || com_num >= COMPORT_MAX)
        {
            pd_error(x, "comport number %d out of range (1-%d)", com_num, COMPORT_MAX);
            return INVALID_HANDLE_VALUE;
        }
#ifdef _MSC_VER
        sprintf_s(buffer, MAX_PATH, "%s%d", x->serial_device_prefix, com_num);
#else
        sprintf(buffer, "%s%d", x->serial_device_prefix, com_num);
#endif
        x->serial_device = gensym(buffer);
    }
    else
    {
#ifdef _MSC_VER
        sprintf_s(buffer, MAX_PATH, "\\\\.\\%s", x->serial_device->s_name); /* assume the slashes were not prefixed by user */
#else
        sprintf(buffer, "\\\\.\\%s", x->serial_device->s_name); /* assume the slashes were not prefixed by user */
#endif
        x->serial_device = gensym(buffer);
    }
    if(x->serial_device && x->serial_device->s_name)
        pretty_name = &x->serial_device->s_name[4];/* skip slashes and dot */

    comport_verbose("[comport] Opening %s", pretty_name);
    fd = CreateFileA( x->serial_device->s_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        0,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        0);

    if(fd == INVALID_HANDLE_VALUE)
    {
        dw = GetLastError();
        switch (dw)
        {
            case 2:
                errStr = "ERROR_FILE_NOT_FOUND";
                break;
            case 3:
                errStr = "ERROR_PATH_NOT_FOUND";
                break;
            case 5:
                errStr = "ERROR_ACCESS_DENIED";
                break;
            case 53:
                errStr = "ERROR_BAD_NETPATH";
                break;
            case 123:
                errStr = "ERROR_INVALID_NAME";
                break;
            default:
                errStr = " ";
                break;
        }
        pd_error(x, "[comport]: could not open device %s:\n failure(%ld) %s\n",
            pretty_name, dw, errStr);
        return INVALID_HANDLE_VALUE;
    }

    /*   Save the Current Port Configuration  */

    if (!GetCommState(fd, &(x->dcb_old)))
    {
        pd_error(x, "[comport]: could not get old dcb of device %s\n", pretty_name);
        CloseHandle(fd);
        return INVALID_HANDLE_VALUE;
    }

    memset(&(x->dcb), 0, sizeof(DCB));

    if (!GetCommState(fd, &(x->dcb)))
    {
        pd_error(x, "[comport]: could not get new dcb of device %s\n", pretty_name);

        CloseHandle(fd);
        return INVALID_HANDLE_VALUE;
    }

    x->dcb.fBinary = TRUE; /*  binary mode, no EOF check  */

    /*   x->dcb.fOutxDsrFlow = FALSE;       DSR output flow control  */
    /*   x->dcb.fDtrControl = DTR_CONTROL_DISABLE;        DTR flow control type  */

    /*   x->dcb.fDsrSensitivity = FALSE;    DSR sensitivity  */

    x->dcb.fErrorChar = FALSE;       /*  enable error replacement  */
    /*    x->dcb.fNull = FALSE;             enable null stripping  */

    /*     DWORD x->dcb.fAbortOnError:1;      abort reads/writes on error  */

    /*    char x->dcb.XonChar;               Tx and Rx XON character  */
    /*    char x->dcb.XoffChar;              Tx and Rx XOFF character  */
    /*    char x->dcb.ErrorChar;             error replacement character  */
    /*    char x->dcb.EofChar;               end of input character  */
    /*    char x->dcb.EvtChar;               received event character  */
    set_bits(x, x->data_bits);      /* CS8 */
    set_stopflag(x, x->stop_bits);  /* ~CSTOPB */
    set_ctsrts(x, x->ctsrts);  /* ~CRTSCTS;*/
    set_xonxoff(x, x->xonxoff); /* (IXON | IXOFF | IXANY) */
    set_baudrate(x, *baud);

    x->comhandle = fd;

    if (com_num == USE_DEVICENAME)
    {
        /* extract index from device name */
        for (i = 0; x->serial_device->s_name[i] != 0; ++i)
            if ((x->serial_device->s_name[i] >= '0') && (x->serial_device->s_name[i] <= '9'))
        com_num = atoi(&x->serial_device->s_name[i]);
    }
    if(set_serial(x))
    {
        comport_verbose("[comport] opened serial line device %d (%s)\n",
            com_num, pretty_name);
    }
    else
    {
        pd_error(x, "[comport] could not set params to control dcb of device %s\n", pretty_name);
        CloseHandle(fd);
        return INVALID_HANDLE_VALUE;
    }

    if (!GetCommTimeouts(fd, &(x->old_timeouts)))
    {
        pd_error(x, "[comport] Couldn't get old timeouts for serial device (%ld)", GetLastError());
    }

    /* setting new timeouts for read to immediately return */
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;

    if (!SetCommTimeouts(fd, &timeouts))
    {
        pd_error(x, "Couldn't set timeouts for serial device (%ld)", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    if (!SetupComm(x->comhandle, 4096L, 4096L))/* try to get big buffers to avoid overruns*/
    {
        pd_error(x, "[comport] Couldn't do SetupComm (%ld)", GetLastError());
    }
    x->comport = com_num;/* output on next tick */
    x->pretty_name = pretty_name;
    return fd;
}

static HANDLE close_serial(t_comport *x)
{
    if(x->comhandle != INVALID_HANDLE_VALUE)
    {
        if (!SetCommState(x->comhandle, &(x->dcb_old)))
        {
            pd_error(x, "[comport] ** ERROR ** couldn't reset params to DCB of device %s\n",
                x->pretty_name);
        }
        if (!SetCommTimeouts(x->comhandle, &(x->old_timeouts)))
        {
            pd_error(x, "[comport] Couldn't reset old_timeouts for serial device");
        }
        CloseHandle(x->comhandle);
        comport_verbose("[comport] closed %s", x->pretty_name);
    }
    return INVALID_HANDLE_VALUE;
}

static int comport_get_dsr(t_comport *x)
{
    short  dsr_state = 0;
    if(x->comhandle != INVALID_HANDLE_VALUE)
    {
        DWORD  modemStat;
        /* get the DSR input state and if it's changed, output it */
        BOOL status = GetCommModemStatus(x->comhandle, &modemStat);
        if (status)
        {
            dsr_state = ((modemStat&MS_DSR_ON)!=0);/* read the DSR input line */
        }
        else
        {
            ;/* call GetLastError, only once so as not to hang pd */
        }
    }
    return dsr_state;
}

int comport_get_cts(t_comport *x)
{
    short cts_state = 0;
    if (x->comhandle != INVALID_HANDLE_VALUE)
    {
        DWORD modemStat;
        /* get the DSR input state and if it's changed, output it */
        BOOL status = GetCommModemStatus(x->comhandle, &modemStat);
        if (status)
        {
            cts_state = ((modemStat&MS_CTS_ON)!=0);/* read the CTS input line */
        }
        else
        {
            ;/* call GetLastError, only once so as not to hang pd */
        }
    }
    return cts_state;
}

#else /* NT */
/* ----------------- POSIX - UNIX ------------------------------ */

static long get_baud_ratebits(t_comport *x, long *baud)
{
    unsigned int i = 0;
    const unsigned int tablelen = sizeof(baudbitstable) / sizeof(*baudbitstable);

    while(i < tablelen && baudbitstable[i].rate > *baud) i++;

    if(baudbitstable[i].rate != *baud)
        pd_error(x, "[comport]: %ld not valid, using closest value: %ld", *baud, baudbitstable[i].rate);

    /* nearest Baudrate finding */
    if(i==tablelen ||  baudbitstable[i].rate < 0)
    {
        pd_error(x, "*Warning* The baud rate %ld is not supported or out of range, using 9600\n",*baud);
	*baud = 9600;
	return B9600;
    }

    *baud =  baudbitstable[i].rate;
    return baudbitstable[i].speedbits;
}

static int set_baudrate(t_comport *x, int ibaud)
{
    struct termios  *tio = &(x->com_termio);
    long            baud = ibaud;
    long        baudbits = get_baud_ratebits(x, &baud);

    comport_verbose("[comport] set_baudrate: Setting baud rate to %g with baudbits 0x%X", baud, baudbits);
    if( cfsetispeed(tio, baudbits) != 0 )
        pd_error(x, "[comport]: ERROR failed to set bitrate: %ld", baudbits);
    if( cfsetospeed(tio, baudbits) != 0 )
        pd_error(x, "[comport]: ERROR failed to set bitrate: %ld", baudbits);

    return baud;
}

/* bits are 5,6,7,8(default) */

static int set_bits(t_comport *x, int nr)
{
    struct termios *tio = &(x->com_termio);
    tio->c_cflag &= ~CSIZE;
    switch(nr)
    {
        case 5: tio->c_cflag |= CS5; return 5;
        case 6: tio->c_cflag |= CS6; return 6;
        case 7: tio->c_cflag |= CS7; return 7;
        default: tio->c_cflag |= CS8;
    }
    return 8;
}


/* 1 ... Parity even, -1 parity odd , 0 (default) no parity */
static int set_parity(t_comport *x, int n)
{
    struct termios *tio = &(x->com_termio);

    switch(n)
    {
        case 1:
            tio->c_cflag |= PARENB;  tio->c_cflag &= ~PARODD; return 1;
        case -1:
            tio->c_cflag |= PARENB | PARODD; return -1;
        default:
            tio->c_cflag &= ~PARENB;
    }
    return 0;
}


/* activate second stop bit with 1, 0(default)*/
static float set_stopflag(t_comport *x, t_float f_nr)
{
    int nr = (int)f_nr;
    struct termios *tio = &(x->com_termio);

    if(nr == 1)
    {
        tio->c_cflag |= CSTOPB;
        return 1;
    }
    else tio->c_cflag &= ~CSTOPB;

    return 0;
}

/* never tested */
static int set_ctsrts(t_comport *x, int nr)
{
    struct termios *tio = &(x->com_termio);

    if(nr == 1)
    {
        tio->c_cflag |= CRTSCTS;
        return 1;
    }
    tio->c_cflag &= ~CRTSCTS;
    return 0;
}

static int set_dtr(t_comport *x, int nr)
{
    int fd = x->comhandle;
    int status;

    if (fd == INVALID_HANDLE_VALUE) return -1;

    ioctl(fd, TIOCMGET, &status);
     if (nr == 0)
        status &= ~TIOCM_DTR;
    else
        status |= TIOCM_DTR;
    ioctl(fd, TIOCMSET, &status);
    return (nr !=0);
}

static int set_rts(t_comport *x, int nr)
{
    int fd = x->comhandle;
    int status;

    if (fd == INVALID_HANDLE_VALUE) return -1;

    ioctl(fd, TIOCMGET, &status);
    if (nr == 0)
        status &= ~TIOCM_RTS;
    else
        status |= TIOCM_RTS;
    ioctl(fd, TIOCMSET, &status);
    return (nr !=0);
}

static int set_xonxoff(t_comport *x, int nr)
{
    struct termios *tio = &(x->com_termio);

    if(nr == 1)
    {
        tio->c_iflag |= (IXON | IXOFF | IXANY);
        return 1;
    }

    tio->c_iflag &= ~IXON & ~IXOFF &  ~IXANY;
    return 0;
}

static int set_hupcl(t_comport *x, int nr)
{
    struct termios  settings;
    int             result;

    result = tcgetattr(x->comhandle, &settings);
    if (result < 0)
    {
        perror ("error in tcgetattr");
        return 0;
    }
    settings.c_iflag &= ~HUPCL;
    if(nr)
    settings.c_iflag |= HUPCL;
    result = tcsetattr(x->comhandle, TCSANOW, &settings);
    if (result < 0)
    {
        pd_error(x,"[comport] could not set HUPCL");
        return 0;
    }
    x->hupcl = nr;
    return 1;
}

static int set_break(t_comport *x, int on)
{
    int fd = x->comhandle;
    int status;

    if (fd == INVALID_HANDLE_VALUE) return -1;

    if (on == 0)
      status = ioctl(fd, TIOCCBRK); // Turn break off, that is, stop sending zero bits.
    else
      status = ioctl(fd, TIOCSBRK); // Turn break on, that is, start sending zero bits.

    return ((status < 0)? status: (on != 0));
}

static int open_serial(unsigned int com_num, t_comport *x)
{
    int             fd;
    unsigned int    i;
    struct termios  *old = &(x->oldcom_termio);
    struct termios  *new = &(x->com_termio);
    int             *baud = &(x->baud);
    glob_t          glob_buffer;

    /* if com_num == USE_DEVICENAME, use device name directly, else try port # */
    if((com_num != USE_DEVICENAME)&&(com_num >= COMPORT_MAX))
    {
        pd_error(x, "[comport] ** WARNING ** port %d not valid, must be between 0 and %d",
            com_num, COMPORT_MAX - 1);
        return INVALID_HANDLE_VALUE;
    }
    /*  post("[comport] globbing %s",x->serial_device_prefix);*/
    /* get the device path based on the port# and the glob pattern */
    switch( glob( x->serial_device_prefix, 0, NULL, &glob_buffer ) )
    {
        case GLOB_NOSPACE:
            pd_error(x,"[comport] out of memory for \"%s\"",x->serial_device_prefix);
            break;
#ifdef GLOB_ABORTED
        case GLOB_ABORTED:
            pd_error(x,"[comport] aborted \"%s\"",x->serial_device_prefix);
            break;
#endif
#ifdef GLOB_NOMATCH
        case GLOB_NOMATCH:
            pd_error(x,"[comport] no serial devices found for \"%s\"",
                     x->serial_device_prefix);
            break;
#endif
    }
    if (com_num == USE_DEVICENAME)
    { /* if possible, find the index of the devicename */
        for (i = 0; i < glob_buffer.gl_pathc; ++i)
        {
            if (0 == strcmp(x->serial_device->s_name, glob_buffer.gl_pathv[i]))
            {
                com_num = i;
                break;
            }
        }
    }
    else if(com_num < glob_buffer.gl_pathc)
        x->serial_device = gensym(glob_buffer.gl_pathv[com_num]);
    else
    {
        pd_error(x, "[comport] ** WARNING ** port #%d does not exist! (max == %lu)",
                 com_num,(unsigned long)(glob_buffer.gl_pathc - 1));
        return INVALID_HANDLE_VALUE;
    }
    globfree( &(glob_buffer) );

    if((fd = open(x->serial_device->s_name, OPENPARAMS)) == INVALID_HANDLE_VALUE)
    {
        pd_error(x, "[comport] ** ERROR ** could not open device %s:\n failure(%d): %s\n",
            x->serial_device->s_name,errno,strerror(errno));
        return INVALID_HANDLE_VALUE;
    }

    /* set no wait on any operation */
    fcntl(fd, F_SETFL, FNDELAY);

    /*   Save the Current Port Configuration  */
    if(tcgetattr(fd, old) == -1 || tcgetattr(fd, new) == -1)
    {
        pd_error(x,"[comport] ** ERROR ** could not get termios-structure of device %s\n",
            x->serial_device->s_name);
        close(fd);
        return INVALID_HANDLE_VALUE;
    }

    /* Setup the new port configuration...NON-CANONICAL INPUT MODE
    .. as defined in termios.h */

    /* enable input and ignore modem controls */
    new->c_cflag |= (CREAD | CLOCAL);
#if 1
    /* always nocanonical, this means raw i/o no terminal */
    new->c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    /* don't process input */
    if(!x->x_inprocess) {
        new->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    }

    /* no post processing */
    new->c_oflag &= ~OPOST;
#else
    /* set to raw mode */
    if(!x->x_inprocess) {
        cfmakeraw(new);
    }
#endif
    /* setup to return after 0 seconds
        ..if no characters are received
        TIME units are assumed to be 0.1 secs */
    /* not needed anymore ??? in new termios in linux
    new->c_cc[VMIN] = 0;
    new->c_cc[VTIME] = 0;
    */

    /* defaults, see input */

    set_bits(x, x->data_bits);      /* CS8 */
    set_stopflag(x, x->stop_bits);  /* ~CSTOPB */
    set_ctsrts(x, x->ctsrts);  /* ~CRTSCTS;*/
    set_xonxoff(x, x->xonxoff); /* (IXON | IXOFF | IXANY) */
    set_baudrate(x, *baud);

    x->x_retry_count = 0; /* reset retry counter */

    if(tcsetattr(fd, TCSAFLUSH, new) != -1)
    {
        comport_verbose("[comport] opened serial line device %d (%s)\n",
            com_num,x->serial_device->s_name);
    }
    else
    {
        pd_error(x,"[comport] ** ERROR ** could not set params to ioctl of device %s\n",
            x->serial_device->s_name);
        close(fd);
        return INVALID_HANDLE_VALUE;
    }
    x->comport = com_num; /* output at next comport_tick */
    x->pretty_name = x->serial_device->s_name;

    return fd;
}

static int close_serial(t_comport *x)
{
    struct termios *tios = &(x->com_termio);
    HANDLE         fd = x->comhandle;

    if(fd != INVALID_HANDLE_VALUE)
    {
        tcsetattr(fd, TCSANOW, tios);
        close(fd);
        comport_verbose("[comport] closed port %i (%s)", x->comport, x->serial_device->s_name);
    }
    return INVALID_HANDLE_VALUE;
}

static int set_serial(t_comport *x)
{
    if(tcsetattr(x->comhandle, TCSAFLUSH, &(x->com_termio)) == -1)
        return 0;
    return 1;
}

static int comport_get_dsr(t_comport *x)
{
    short  dsr_state = 0;

    if (x->comhandle != INVALID_HANDLE_VALUE)
    {
        int status;/*dsr outlet*/
        /* get the DSR input state and if it's changed, output it */
        ioctl(x->comhandle, TIOCMGET, &status);/*dsr outlet*/
        dsr_state = ((status&TIOCM_LE)!=0);/* read the DSR input line */
    }
    return dsr_state;
}

int comport_get_cts(t_comport *x)
{
    short  cts_state = 0;

    if (x->comhandle != INVALID_HANDLE_VALUE)
    {
        int status;/*cts outlet*/
        /* get the CTS input state and if it's changed, output it */
        ioctl(x->comhandle, TIOCMGET, &status);
        cts_state = ((status&TIOCM_CTS)!=0);/* read the CTS input line */
    }
    return cts_state;
}

#endif /* else NT */

/* ------------------- serial pd methods --------------------------- */
static void comport_pollintervall(t_comport *x, t_floatarg g)
{
    if (g < 1) g = 1;
    x->x_deltime = g;
}

static void comport_retries(t_comport *x, t_floatarg g)
{
    if (g < 0) g = 0;
    x->x_retries = g;
}

static void comport_tick(t_comport *x)
{
#ifdef _WIN32
    HANDLE       fd = x->comhandle;
#else
    int  fd = x->comhandle;
#endif /* _WIN32 */
    int          err;

    x->x_hit = 0;

    if(fd != INVALID_HANDLE_VALUE)
    { /* while there are bytes, read them and send them out, ignore errors (!??) */
#ifdef _WIN32
        DWORD           dwRead;
        OVERLAPPED      osReader;
        DWORD           dwX;
        DWORD           whicherr = 0;

        err = 0;
/* initialize all fields off osReader to zero to avoid gcc warnings about */
/* missing initializer if we just do osReader ={0} */
        osReader.hEvent = 0;
        osReader.Internal = 0;
        osReader.InternalHigh = 0;
        osReader.Offset = 0;
        osReader.OffsetHigh = 0;
        /*osReader.Pointer = 0; seems MinGW doesn't knoow about this one */
        osReader.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if(ReadFile(x->comhandle, x->x_inbuf, x->x_inbuf_len, &dwRead, &osReader))
        //if (ReadFile(x->comhandle, x->x_inbuf, NULL, &dwRead, &osReader))
        {
            if(dwRead > 0)
            {
                for(dwX = 0; dwX < dwRead ;dwX++)
                {
                    outlet_float(x->x_data_outlet, (t_float) x->x_inbuf[dwX]);
                }
            }
        }
        else
        {
            whicherr = GetLastError();
            if (whicherr == ERROR_IO_PENDING)
            {
                //post("iopending\n");
                if (GetOverlappedResult(x->comhandle, &osReader, &dwRead, FALSE)) // don't wait
                {
                    //post("dwRead %ld\n", dwRead);
                    if (dwRead > 0)
                    {
                        for (dwX = 0; dwX < dwRead;dwX++)
                        {
                            outlet_float(x->x_data_outlet, (t_float)x->x_inbuf[dwX]);
                        }
                    }
                }
                else
                {
                    whicherr = GetLastError();
                    if (whicherr != ERROR_IO_PENDING) err = -1;
                }
            }
            else err = -1;
        }
        CloseHandle(osReader.hEvent);
#else
        fd_set          com_rfds;
        int             count = 0;
        int             i;
        long int        whicherr = 0;

        FD_ZERO(&com_rfds);
        FD_SET(fd,&com_rfds);
        while((err = select(fd+1, &com_rfds, NULL, NULL, &null_tv)) > 0)
        {
            ioctl(fd, FIONREAD, &count); /* load count with the number of bytes in the receive buffer... */
            if (count > x->x_inbuf_len) count = x->x_inbuf_len; /* ...but no more than the buffer can hold */
            /*err = read(fd,(char *) &serial_byte,1);*/
            err = read(fd,(char *)x->x_inbuf, count);/* try to read count bytes */
            if (err > 0)
            {
                for (i = 0; i < err; ++i )
                {
                    outlet_float(x->x_data_outlet, (t_float) x->x_inbuf[i]);
                }
            }
            else if (err == 0 && count == 0)
            {
                /* if both ioctl() and read() return a 0 count, assume
                 * that we lost the connection to the serial port.
                 * otherwise there is a race condition when the serial
                 * port gets interrupted, like if the USB gets yanked
                 * out or a bluetooth connection drops */
                if(x->x_retry_count < x->x_retries)
                {
                    t_atom retrying_atom;
                    SETFLOAT(&retrying_atom, x->x_retry_count);
                    outlet_anything(x->x_status_outlet, gensym("retrying"), 1, &retrying_atom);

                    pd_error(x, "[comport]: lost connection to port %i (%s), retrying...",
                             x->comport, x->serial_device->s_name);
                    if (!x->x_hit)
                        clock_delay(x->x_clock, 1000); /* retry every second */
                    x->x_retry_count++;
                    return;
                }
                else
                {
                    pd_error(x, "[comport]: Giving up on port %i (%s)!",
                             x->comport, x->serial_device->s_name);
                    comport_close(x);
                }
            }
            else
                whicherr = errno;
        }
#endif /* _WIN32 */
        if(err < 0)
        { /* if a read error detected */
            if(x->rxerrors < 10) /* ten times max */
                pd_error(x, "[comport]: RXERRORS on serial line (%ld)\n", (long int)whicherr);
            x->rxerrors++; /* remember */
        }
/* now if anything to send, send the output buffer */
        if (0 != x->x_outbuf_wr_index)
        {
#ifdef _WIN32
            OVERLAPPED osWrite;
            DWORD      dwWritten;
            DWORD      dwToWrite = (DWORD)x->x_outbuf_wr_index;
            DWORD      dwErr;
            DWORD      numTransferred = 0L;

/* initialize all fields off osWrite to zero to avoid gcc warnings about */
/* missing initializer if we just do osWrite ={0} */
            osWrite.hEvent = 0;
            osWrite.Internal = 0;
            osWrite.InternalHigh = 0;
            osWrite.Offset = 0;
            osWrite.OffsetHigh = 0;
            /*osWrite.Pointer = 0; seems MinGW doesn't know about this one */
            osWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (osWrite.hEvent == NULL)
            {
                pd_error(x, "[comport]: Couldn't create event. Transmission aborted.");
                goto endsendevent;
            }

            else if (!WriteFile(x->comhandle, x->x_outbuf, dwToWrite, &dwWritten, &osWrite))
            {
                dwErr = GetLastError();
                if (dwErr != ERROR_IO_PENDING)
                {
                    pd_error(x, "[comport]: WriteFile error: %d", (int)dwErr);
                    goto endsendevent;
                }
            }
            if (!GetOverlappedResult(x->comhandle, &osWrite, &numTransferred, TRUE))
            {/* wait for the character(s) to be sent */
                dwErr = GetLastError();
                pd_error(x, "[comport]: WriteFile:GetOverlappedResult error: %d", (int)dwErr);
            }
endsendevent:
            CloseHandle(osWrite.hEvent);
#else
            err = write(x->comhandle,(char *)x->x_outbuf, x->x_outbuf_wr_index);
            if (err != x->x_outbuf_wr_index)
            {
                pd_error(x,"[comport]: Write failed for %d bytes, error is %d",err,errno);
            }
#endif /*_WIN32*/
            x->x_outbuf_wr_index = 0; /* for now we just drop anything that didn't send */
        }
        if (!x->x_hit) clock_delay(x->x_clock, x->x_deltime); /* default 10 ms */
    }
}

static int write_serial(t_comport *x, unsigned char  serial_byte)
{
    if(x->comhandle == INVALID_HANDLE_VALUE)
    {
        comport_verbose ("[comport]: Serial port is not open");
        return 0;
    }
    else if(x->x_outbuf_wr_index < x->x_outbuf_len)
    {
        x->x_outbuf[x->x_outbuf_wr_index++] = serial_byte;
        return 1;
    }
    /* handle overrun error */
    pd_error (x, "[comport]: buffer is full");
    return 0;
}

static int write_serials(t_comport *x, unsigned char *serial_buf, int buf_length)
{
    int i;
    if(x->comhandle == INVALID_HANDLE_VALUE)
    {
        pd_error (x, "[comport]: Serial port is not open");
        return 0;
    }
    for (i = 0; ((i < buf_length) && (x->x_outbuf_wr_index < x->x_outbuf_len)); ++x->x_outbuf_wr_index, ++i)
        x->x_outbuf[x->x_outbuf_wr_index] = serial_buf[i];
    if (i != buf_length) pd_error (x, "[comport]: buffer is full");
    return i;
}

static void comport_float(t_comport *x, t_float f)
{
    unsigned char serial_byte = ((int) f) & 0xFF; /* brutal conv */

    if (write_serial(x,serial_byte) != 1)
    {
        pd_error(x, "Write error, maybe TX-OVERRUNS on serial line");
    }
}

static void comport_list(t_comport *x, t_symbol *s, int argc, t_atom *argv)
{
    unsigned char   temp_array[COMPORT_BUF_SIZE];/* arbitrary maximum list length */
    int             i, count;
    (void)s; /* squelch unused-parameter warning */

    count = argc;
    if (argc > COMPORT_BUF_SIZE)
    {
        pd_error (x, "[comport] truncated list of %d elements to %d", argc, count);
        count = COMPORT_BUF_SIZE;
    }
    for(i = 0; i < count; i++)
        temp_array[i] = ((unsigned char)atom_getint(argv+i))&0xFF; /* brutal conv */
    write_serials(x, temp_array, count);
}

static void *comport_new(t_symbol *s, int argc, t_atom *argv)
{
    t_comport test;
    t_comport *x;
    HANDLE    fd;
    const char *serial_device_prefix;
    int com_num = 0;
    int ibaud = 9600;
    (void)s; /* squelch unused-parameter warning */

#ifdef _WIN32
/* According to http://msdn2.microsoft.com/en-us/library/aa363858.aspx To
specify a COM port number greater than 9, use the following syntax:
"\\\\.\\COM10".  This syntax works for all port numbers and hardware that
allows COM port numbers to be specified. */
    serial_device_prefix = "\\\\.\\COM";
/* for UNIX, this is a glob pattern for matching devices  */
#elif defined __APPLE__
    serial_device_prefix = "/dev/tty.*";
#elif defined __linux__
    /* serial: ttyS?    USB-serial: ttyUSB?   USB-CDC: ttyACM? */
    serial_device_prefix = "/dev/tty[ASU]*";
#elif defined IRIX
    serial_device_prefix = "/dev/ttyd*";
#endif /* __linux__ */

    if(argc > 0) {
      if (argv->a_type == A_FLOAT)
        com_num = (int)atom_getfloatarg(0, argc, argv);
      else
        serial_device_prefix = atom_getsymbol(argv)->s_name;
    }
    if(argc > 1)
        ibaud = atom_getfloatarg(1, argc, argv);

/*	 Open the Comport for RD and WR and get a handle */
/* this line should use a real serial device */
#ifdef _WIN32
    strncpy_s(test.serial_device_prefix, strlen(serial_device_prefix) + 1, serial_device_prefix, strlen(serial_device_prefix) + 1);
#else
    strncpy(test.serial_device_prefix, serial_device_prefix, MAXPDSTRING-1);
    test.serial_device_prefix[MAXPDSTRING-1] = 0;
#endif
    test.baud = ibaud;
    test.data_bits = 8; /* default 8 data bits */
    test.parity_bit = 0;/* default no parity bit */
#ifdef _WIN32
    test.stop_bits = 1;/* default 1 stop bit */
#else
    test.stop_bits = 0;/* default 1 stop bit */
#endif /* _WIN32 */
    test.ctsrts = 0; /* default no hardware handshaking */
    test.xonxoff = 0; /* default no software handshaking */
    test.hupcl = 1; /* default hangup on close */

    /* don't try to open negative devices */
    if(com_num < 0) {
      fd = INVALID_HANDLE_VALUE;
    } else {
      fd = open_serial((unsigned int)com_num, &test);
    }

    /* Now  nothing really bad could happen so we create the class */
    x = (t_comport *)pd_new(comport_class);

    x->comport = test.comport;/* com_num */
#ifdef _WIN32
    strncpy_s(x->serial_device_prefix, strlen(serial_device_prefix) + 1, serial_device_prefix, strlen(serial_device_prefix) + 1);
#else
    strncpy(x->serial_device_prefix, serial_device_prefix, MAXPDSTRING);
#endif
    x->serial_device = test.serial_device; /* we need this so 'help' doesn't crash */

    x->baud = test.baud;
    x->data_bits = test.data_bits;
    x->parity_bit = test.parity_bit;
    x->stop_bits = test.stop_bits;
    x->ctsrts = test.ctsrts;
    x->xonxoff = test.xonxoff;
    x->hupcl = test.hupcl;
    x->comhandle = fd; /* holds the comport handle */

    if(fd == INVALID_HANDLE_VALUE && com_num>=0)
    {
        pd_error(x, "[comport] opening serial port %d failed!", com_num);
    }
    else
    {
#ifdef _WIN32
        memcpy(&(x->dcb_old), &(test.dcb_old), sizeof(DCB)); /*  save the old com config  */
        memcpy(&(x->dcb), &(test.dcb), sizeof(DCB)); /*  for the new com config  */
#else
        /* save the old com and new com config */
        bcopy(&(test.oldcom_termio),&(x->oldcom_termio),sizeof(struct termios));
        bcopy(&(test.com_termio),&(x->com_termio),sizeof(struct termios));
#endif
    }

/* allocate memory for in and out buffers */
    x->x_inbuf = getbytes(COMPORT_BUF_SIZE);
    if (NULL == x->x_inbuf)
    {
        pd_error(x, "[comport] unable to allocate input buffer");
        return 0;
    }
    x->x_inbuf_len = COMPORT_BUF_SIZE;
    x->x_outbuf = getbytes(COMPORT_BUF_SIZE);
    if (NULL == x->x_outbuf)
    {
        pd_error(x, "[comport] unable to allocate output buffer");
        return 0;
    }
    x->x_outbuf_len = COMPORT_BUF_SIZE;
    x->x_outbuf_wr_index = 0;

    x->rxerrors = 0; /* holds the rx line errors */

    x->x_data_outlet = outlet_new(&x->x_obj, &s_float);
    x->x_status_outlet = outlet_new(&x->x_obj, &s_float);

    x->x_hit = 0;
    /* Since 10ms is also the default poll time for most HID devices,
     * and it seems that for most uses of [comport] (i.e. arduinos and
     * other serial ports 115200 baud or less) that 10ms polling is
     * going to give the data as fast as 1ms polling with a lot less
     * CPU time wasted. */
    x->x_deltime = 10;
    x->x_retries = 10;
    x->x_clock = clock_new(x, (t_method)comport_tick);

    clock_delay(x->x_clock, x->x_deltime);

    x->x_verbose = 0;
    x->x_inprocess = 0;

    return x;
}


static void comport_free(t_comport *x)
{
    comport_verbose("[comport] free serial...");
    clock_unset(x->x_clock);
    clock_free(x->x_clock);
    x->comhandle = close_serial(x);
    freebytes(x->x_inbuf, x->x_inbuf_len);
    freebytes(x->x_outbuf, x->x_outbuf_len);
}

/* ---------------- use serial settings ------------- */

static void comport_baud(t_comport *x,t_floatarg f)
{
    if(f == x->baud)
    {
        comport_verbose("[comport] baudrate already %d\n",x->baud);
        return;
    }

    x->baud = set_baudrate(x,f);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(set_serial(x) == 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set baudrate of device %s\n", x->pretty_name);
    }
    else comport_verbose("[comport] set baudrate of %s to %d\n", x->pretty_name, x->baud);
}

static void comport_bits(t_comport *x,t_floatarg fbits)
{
    int bits = set_bits(x,fbits);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(set_serial(x) == 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set bits of device %s\n", x->pretty_name);
        return;
    }
    else comport_verbose("[comport] set bits of %s to %d\n", x->pretty_name, bits);
    x->data_bits = bits;
}


static void comport_parity(t_comport *x,t_floatarg fparity)
{
    int parity = set_parity(x, fparity);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(set_serial(x) == 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set extra paritybit of device %s\n", x->pretty_name);
        return;
    }
    else comport_verbose("[comport] set extra paritybit of %s to %d\n", x->pretty_name, parity);
    x->parity_bit = parity;
}

static void comport_stopbit(t_comport *x, t_floatarg fstop)
{
    t_float stop = set_stopflag(x, fstop);
    const char*stopname =
#ifdef _WIN32
      "stopbits"
#else
      "extra stopbit"
#endif
      ;


    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(set_serial(x) == 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set %s of device %s\n",
            stopname, x->pretty_name);
        return;
    }
    else comport_verbose("[comport] set %s of %s to %g\n", stopname, x->pretty_name, stop);
    x->stop_bits = stop;
}

static void comport_rtscts(t_comport *x,t_floatarg f)
{
    f = set_ctsrts(x,f);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(set_serial(x) == 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set rts_cts of device %s\n", x->pretty_name);
        return;
    }
    else comport_verbose("[comport] set rts-cts of %s to %g\n", x->pretty_name, f);
    x->ctsrts = f;
}

static void comport_dtr(t_comport *x,t_floatarg f)
{
    f = set_dtr(x,f);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(f < 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set dtr of device %s\n", x->pretty_name);
    }
    else comport_verbose("[comport] set dtr of %s to %g\n", x->pretty_name, f);
}

static void comport_rts(t_comport *x,t_floatarg f)
{
    f = set_rts(x,f);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(f < 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set rts of device %s\n", x->pretty_name);
    }
    else comport_verbose("[comport] set rts of %s to %g\n", x->pretty_name, f);
}

static void comport_break(t_comport *x,t_floatarg f)
{
    f = set_break(x,f);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(f < 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set break of device %s\n", x->pretty_name);
    }
    else comport_verbose("[comport] set break of %s to %g\n", x->pretty_name, f);
}

static void comport_xonxoff(t_comport *x,t_floatarg f)
{
    f = set_xonxoff(x,f);

    if(x->comhandle == INVALID_HANDLE_VALUE)return;

    if(set_serial(x) == 0)
    {
        pd_error(x,"[comport] ** ERROR ** could not set xonxoff of device %s\n", x->pretty_name);
        return;
    }
    else comport_verbose("[comport] set xonxoff of %s to %g\n", x->pretty_name, f);
    x->xonxoff = f;
}

static void comport_hupcl(t_comport *x,t_floatarg f)
{
	set_hupcl(x, f);
}

static void comport_close(t_comport *x)
{
    clock_unset(x->x_clock);
    x->x_hit = 1;
    x->comhandle = close_serial(x);
    x->comport = -1; /* none */
    if (x->x_status_outlet != NULL) outlet_float(x->x_status_outlet, (float)x->comport);
}

static void comport_open(t_comport *x, t_floatarg f)
{
    if(x->comhandle != INVALID_HANDLE_VALUE)
        comport_close(x);

    x->comhandle = open_serial(f,x);

    clock_delay(x->x_clock, x->x_deltime);
}

/*
   dangerous but if you really have some stupid devicename ...
   you can open any file on systems if suid is set on pd be careful
*/

static void comport_devicename(t_comport *x, t_symbol *s)
{
    x->serial_device = s;
    if(x->comhandle != INVALID_HANDLE_VALUE)
        comport_close(x);

    x->comhandle = open_serial(USE_DEVICENAME,x);
    clock_delay(x->x_clock, x->x_deltime);
}

static void comport_print(t_comport *x, t_symbol *s, int argc, t_atom *argv)
{
    static char buf[256];
    char        *pch = buf;
    (void)s; /* squelch unused-parameter warning */

    while(argc--)
    {
        atom_string(argv++, buf, 255);
        while(*pch != 0)
        {
            write_serial(x, *pch++);
        }
        if(argc > 0)
        {
            write_serial(x, ' ');
        }
    }
}

static void comport_enum(t_comport *x)
{
#ifdef _WIN32
    HANDLE          fd;
    char            device_name[10];
    unsigned int    i;
    DWORD           dw;
    for(i = 1; i < COMPORT_MAX; i++)
    {
#ifdef _MSC_VER
        sprintf_s(device_name, 10, "%s%d", x->serial_device_prefix, i);
#else
        sprintf(device_name, "%s%d", x->serial_device_prefix, i);
#endif
        fd = CreateFileA( device_name,
                GENERIC_READ | GENERIC_WRITE,
                0,
                0,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                0);
        dw = 0L;
        if(fd == INVALID_HANDLE_VALUE)
            dw = GetLastError();
        else
            CloseHandle(fd);
        if (dw == 0)post("\t%d - COM%d (free)", i, i);
        else if (dw == ERROR_ACCESS_DENIED)pd_error(x, "\t%d - COM%d (in use)", i, i);
    }
#else
    unsigned int i;
    glob_t         glob_buffer;
    int            fd;
    struct termios test;

/* first look for registered devices in the filesystem */
    switch( glob( x->serial_device_prefix, 0, NULL, &glob_buffer ) )
    {
    case GLOB_NOSPACE:
        pd_error(x,"[comport] out of memory for \"%s\"",x->serial_device_prefix);
        break;
# ifdef GLOB_ABORTED
        case GLOB_ABORTED:
        pd_error(x,"[comport] aborted \"%s\"",x->serial_device_prefix);
        break;
# endif /* GLOB_ABORTED */
# ifdef GLOB_NOMATCH
    case GLOB_NOMATCH:
        pd_error(x,"[comport] no serial devices found for \"%s\"",x->serial_device_prefix);
        break;
# endif /* GLOB_NOMATCH */
    }
    for(i=0; i<glob_buffer.gl_pathc; i++)
    {
/* now try to open the device */
        if((fd = open(glob_buffer.gl_pathv[i], OPENPARAMS)) != INVALID_HANDLE_VALUE)
        {
/* now see if it has attributes */
            if ((tcgetattr(fd, &test)) != -1)
                post("\t%d\t%s", i, glob_buffer.gl_pathv[i]);// this one really exists
            close (fd);
        }
    }
#endif  /* _WIN32 */
}

static void comport_ports(t_comport *x)
{ /* the same as comport_enum except outputs list of available ports on status outlet */
    unsigned int    i;
    t_atom          output_atom[2];
#ifdef _WIN32
    HANDLE          fd;
    char            device_name[10];
    DWORD           dw;

    for(i = 1; i < COMPORT_MAX; i++)
    {
#ifdef _MSC_VER
        sprintf_s(device_name, 10, "%s%d", x->serial_device_prefix, i);
#else
        sprintf(device_name, "%s%d", x->serial_device_prefix, i);
#endif
        fd = CreateFileA( device_name,
                GENERIC_READ | GENERIC_WRITE,
                0,
                0,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                0);
        dw = 0L;
        if(fd == INVALID_HANDLE_VALUE)
            dw = GetLastError();
        else
            CloseHandle(fd);
        if ((dw == 0)||(dw == ERROR_ACCESS_DENIED))
        { /* output index and name as a list */
            SETFLOAT(&output_atom[0], i);
            SETSYMBOL(&output_atom[1], gensym(&device_name[4]));/* strip the slashes and dot */
            outlet_anything( x->x_status_outlet, gensym("ports"), 2, output_atom);
        }
    }
#else
    glob_t          glob_buffer;
    int             fd;
    struct termios  test;

/* first look for registered devices in the filesystem */
    switch( glob( x->serial_device_prefix, 0, NULL, &glob_buffer ) )
    {
        case GLOB_NOSPACE:
            pd_error(x,"[comport] out of memory for \"%s\"",x->serial_device_prefix);
            break;
# ifdef GLOB_ABORTED
        case GLOB_ABORTED:
            pd_error(x,"[comport] aborted \"%s\"",x->serial_device_prefix);
            break;
# endif /* GLOB_ABORTED */
# ifdef GLOB_NOMATCH
        case GLOB_NOMATCH:
            pd_error(x,"[comport] no serial devices found for \"%s\"",x->serial_device_prefix);
            break;
# endif /* GLOB_NOMATCH */
    }
    for(i = 0; (i < glob_buffer.gl_pathc) && (i < COMPORT_MAX); i++)
    {
/* now try to open the device */
        if((fd = open(glob_buffer.gl_pathv[i], OPENPARAMS)) != INVALID_HANDLE_VALUE)
        {
/* now see if it has attributes */
            if ((tcgetattr(fd, &test)) != -1)
            { /* output index and name as a list */
                SETFLOAT(&output_atom[0], i);
                SETSYMBOL(&output_atom[1], gensym(glob_buffer.gl_pathv[i]));
                outlet_anything( x->x_status_outlet, gensym("ports"), 2, output_atom);
            }
            close (fd);
        }
    }
#endif  /* _WIN32 */
}

static void comport_output_print(t_comport *x)
{
    post("[comport]: available serial ports:");
    comport_enum(x);
}


static void comport_output_status(t_comport *x, t_symbol *selector, t_float output_value)
{
    t_atom *output_atom = getbytes(sizeof(t_atom));
    SETFLOAT(output_atom, output_value);
    outlet_anything( x->x_status_outlet, selector, 1, output_atom);
    freebytes(output_atom,sizeof(t_atom));
}

static void comport_output_port_status(t_comport *x)
{
    comport_output_status(x, gensym("port"), (float)x->comport);
}

static void comport_output_dsr_status(t_comport *x)
{
    comport_output_status(x, gensym("dsr"), (float)comport_get_dsr(x));
}

static void comport_output_cts_status(t_comport *x)
{
    comport_output_status(x, gensym("cts"), (float)comport_get_cts(x));
}

static void comport_output_baud_rate(t_comport *x)
{
    comport_output_status(x, gensym("baud"), x->baud);
}

static void comport_output_parity_bit(t_comport *x)
{
    comport_output_status(x, gensym("parity"), x->parity_bit);
}

static void comport_output_stop_bits(t_comport *x)
{
#ifdef _WIN32
    comport_output_status(x, gensym("stop"), x->stop_bits);
#else
    comport_output_status(x, gensym("stop"), x->stop_bits+1);
#endif
}

static void comport_output_data_bits(t_comport *x)
{
    comport_output_status(x, gensym("data"), x->data_bits);
}

static void comport_output_rtscts(t_comport *x)
{
    comport_output_status(x, gensym("rtscts"), x->ctsrts);
}

static void comport_output_xonxoff(t_comport *x)
{
    comport_output_status(x, gensym("xonxoff"), x->xonxoff);
}

static void comport_output_hupcl(t_comport *x)
{
    comport_output_status(x, gensym("hupcl"), x->hupcl);
}

static void comport_output_rxerrors(t_comport *x)
{
    comport_output_status(x, gensym("rxerrors"), x->rxerrors);
}

static void comport_output_open_status(t_comport *x)
{
    if(x->comhandle == INVALID_HANDLE_VALUE)
        comport_output_status(x, gensym("open"), 0);
    else
        comport_output_status(x, gensym("open"), 1);
}

static void comport_devices(t_comport *x)
{
    comport_output_print(x);
}

static void comport_info(t_comport *x)
{
    comport_output_open_status(x);
    comport_output_port_status(x);
    comport_output_baud_rate(x);
    comport_output_dsr_status(x);
    comport_output_cts_status(x);
    comport_output_parity_bit(x);
    comport_output_stop_bits(x);
    comport_output_data_bits(x);
    comport_output_rtscts(x);
    comport_output_xonxoff(x);
    comport_output_hupcl(x);
    comport_output_rxerrors(x);
}

/* ---------------- HELPER ------------------------- */
static void comport_set_verbose(t_comport *x, t_floatarg f)
{
    x->x_verbose = f;
    comport_verbose("[comport] verbose is on: %d", x->x_verbose);
}
static void comport_set_inprocess(t_comport *x, t_floatarg f)
{
  x->x_inprocess = (f>=1.);
    comport_verbose("[comport] input processing for newly opened devices is %s",
        x->x_inprocess?"on":"off");
}

static void comport_help(t_comport *x)
{
    post("[comport] serial port %d (baud %d):", x->comport, x->baud);
    if(x->comport >= 0 && x->comport < COMPORT_MAX)
    {
        post("\tdevicename: %s", x->pretty_name);
    }

    post("  Methods:");
    post("   baud <baud>       ... set baudrate to nearest possible baud\n"
         "   bits <bits>       ... set number of bits (7 or 8)\n"
         "   stopbit <0|1>     ... set off|on stopbit\n"
         "   rtscts <0|1>      ... set rts/cts off|on\n"
         "   parity <0|1>      ... set parity off|on\n"
         "   xonxoff <0|1>     ... set xon/xoff off|on\n"
         "   dtr <0|1>         ... set dtr off|on\n"
         "   rts <0|1>         ... set rts off|on\n"
         "   hupcl <0|1>       ... set hang-up on close off|on\n"
         "   close             ... close device\n"
         "   open <num>        ... open device number num\n"
         "   devicename <d>    ... set device name to d (eg. /dev/ttyS8)\n"
         "   print <list>      ... print list of atoms on serial\n"
         "   pollintervall <t> ... set poll interval to t ticks\n"
         "   verbose <level>   ... for debug set verbosity to level\n"
         "   inprocess <0|1>   ... set input-processing off|on\n"
         "   info              ... output info on status outlet\n"
         "   devices           ... post list of available devices\n"
         "   ports             ... output list of available devices on status outlet\n"
         "   help              ... post this help");
}

/* ---------------- SETUP OBJECTS ------------------ */
void comport_setup(void)
{
    comport_class = class_new(gensym("comport"), (t_newmethod)comport_new,
        (t_method)comport_free, sizeof(t_comport),
        0, A_GIMME, 0);

    class_addfloat(comport_class, (t_method)comport_float);
    class_addlist(comport_class, (t_method)comport_list);
    /*
        class_addbang(comport_class, comport_bang
    */
    class_addmethod(comport_class, (t_method)comport_baud, gensym("baud"),A_FLOAT, 0);

    class_addmethod(comport_class, (t_method)comport_bits, gensym("bits"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_stopbit, gensym("stopbit"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_rtscts, gensym("rtscts"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_dtr, gensym("dtr"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_rts, gensym("rts"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_break, gensym("break"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_parity, gensym("parity"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_xonxoff, gensym("xonxoff"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_hupcl, gensym("hupcl"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_close, gensym("close"), 0);
    class_addmethod(comport_class, (t_method)comport_open, gensym("open"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_devicename, gensym("devicename"), A_SYMBOL, 0);
    class_addmethod(comport_class, (t_method)comport_print, gensym("print"), A_GIMME, 0);
    class_addmethod(comport_class, (t_method)comport_pollintervall, gensym("pollintervall"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_retries, gensym("retries"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_set_verbose, gensym("verbose"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_set_inprocess, gensym("inputprocess"), A_FLOAT, 0);
    class_addmethod(comport_class, (t_method)comport_help, gensym("help"), 0);
    class_addmethod(comport_class, (t_method)comport_info, gensym("info"), 0);
    class_addmethod(comport_class, (t_method)comport_devices, gensym("devices"), 0);
    class_addmethod(comport_class, (t_method)comport_ports, gensym("ports"), 0);

#ifndef _WIN32
    null_tv.tv_sec = 0; /* no wait */
    null_tv.tv_usec = 0;
#endif /* NOT _WIN32 */
    logpost(0, PD_DEBUG, "comport - Pd external for unix/windows\n"
        "LGPL-2.1+ 1998-2022,  Winfried Ritsch and others (see LICENSE.txt)\n"
        "Institute for Electronic Music - Graz");
}
