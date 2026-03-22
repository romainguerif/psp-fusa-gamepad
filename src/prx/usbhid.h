#include "usb.h"

#define PSP_USB_HID			"FUSAGAMEPAD"
#define PSP_USB_GAMEPAD_PID	0x01D2 //FuSa v3.2

#define DATA_SIZE			8

/* Data layout — matches DS3/DS4 style
   Byte 0: stick X
   Byte 1: stick Y
   Byte 2: stick Z  (D-pad L/R as axis)
   Byte 3: stick Rz (D-pad U/D as axis)
   Byte 4: buttons 1-8  (low byte)
   Byte 5: buttons 9-16 (high byte)
   Byte 6-7: unused
*/
#define AX_X		0
#define AX_Y		1
#define AX_Z		2
#define AX_RZ		3
#define BTN_LO		4
#define BTN_HI		5


extern int usb_start (void);
extern int usb_stop (void);

/* HID Report Descriptor — based on DualShock 3 (proven on macOS)
   All 4 axes in single Physical collection + hat switch + 16 buttons */
char ReportDescriptorGamepad[] __attribute__ ((aligned(64))) = {
	0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
	0x09, 0x05,        // USAGE (Game Pad)
	0xA1, 0x01,        // COLLECTION (Application)

	// --- 4 axes in single Physical collection (bytes 0-3) ---
	0x09, 0x01,        //   USAGE (Pointer)
	0xA1, 0x00,        //   COLLECTION (Physical)
	0x09, 0x30,        //     USAGE (X)  — stick L/R
	0x09, 0x31,        //     USAGE (Y)  — stick U/D
	0x09, 0x32,        //     USAGE (Z)  — dpad L/R
	0x09, 0x35,        //     USAGE (Rz) — dpad U/D
	0x15, 0x00,        //     LOGICAL_MINIMUM (0)
	0x26, 0xFF, 0x00,  //     LOGICAL_MAXIMUM (255)
	0x35, 0x00,        //     PHYSICAL_MINIMUM (0)
	0x46, 0xFF, 0x00,  //     PHYSICAL_MAXIMUM (255)
	0x75, 0x08,        //     REPORT_SIZE (8)
	0x95, 0x04,        //     REPORT_COUNT (4)
	0x81, 0x02,        //     INPUT (Data,Var,Abs)
	0xC0,              //   END_COLLECTION (Physical)

	// --- 16 buttons (bytes 4-5) ---
	0x05, 0x09,        //   USAGE_PAGE (Button)
	0x19, 0x01,        //   USAGE_MINIMUM (Button 1)
	0x29, 0x10,        //   USAGE_MAXIMUM (Button 16)
	0x15, 0x00,        //   LOGICAL_MINIMUM (0)
	0x25, 0x01,        //   LOGICAL_MAXIMUM (1)
	0x75, 0x01,        //   REPORT_SIZE (1)
	0x95, 0x10,        //   REPORT_COUNT (16)
	0x81, 0x02,        //   INPUT (Data,Var,Abs)

	// --- Padding (bytes 6-7) ---
	0x75, 0x08,        //   REPORT_SIZE (8)
	0x95, 0x02,        //   REPORT_COUNT (2)
	0x81, 0x03,        //   INPUT (Constant)

	0xC0               // END_COLLECTION (Application)
};
/*
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x05,                    // USAGE (Game Pad)
    0xa1, 0x01,                    // COLLECTION (Application)
    0x09, 0x01,                    //   USAGE (Pointer)
    0xa1, 0x00,                    //   COLLECTION (Physical)
    0x09, 0x30,                    //     USAGE (X)
    0x09, 0x31,                    //     USAGE (Y)
    0x09, 0x32,                    //     USAGE (Z)
    0x09, 0x35,                    //     USAGE (Rz)
    0x15, 0xff,                    //     LOGICAL_MINIMUM (-1)
    0x25, 0x01,                    //     LOGICAL_MAXIMUM (1)
    0x95, 0x08,                    //     REPORT_COUNT (8)
    0x75, 0x04,                    //     REPORT_SIZE (4)
    0x81, 0x02,                    //     INPUT (Data,Var,Abs)
    0xc0,                          // END_COLLECTION
    0x09, 0x39,                    //   USAGE (Hat switch)
    0x15, 0x00,                    //     LOGICAL_MINIMUM (0)
    0x25, 0x03,                    //     LOGICAL_MAXIMUM (3)
    0x35, 0x00,                    //     PHYSICAL_MINIMUM (0)
    0x46, 0x0e, 0x01,              //     PHYSICAL_MAXIMUM (270)
    0x65, 0x14,                    //     UNIT (Eng Rot:Angular Pos)
    0x75, 0x04,                    //     REPORT_SIZE (4)
    0x95, 0x01,                    //     REPORT_COUNT (1)
    0x81, 0x02,                    //     INPUT (Data,Var,Abs)
    0x05, 0x09,                    //   USAGE_PAGE (Button)
    0x19, 0x01,                    //   USAGE_MINIMUM (Button 1)
    0x29, 0x0c,                    //     USAGE_MAXIMUM (Button 12)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x25, 0x01,                    //   LOGICAL_MAXIMUM (1)
    0x95, 0x0c,                    //     REPORT_COUNT (12)
    0x75, 0x01,                    //   REPORT_SIZE (1)
    0x81, 0x02,                    //   INPUT (Data,Var,Abs)
    0xc0                           // END_COLLECTION
};

 Endpoint blocks */
// control Ep0 and our Ep1
static
struct UsbEndpoint endpoints[2] = {
  { 0, 0, 0 },
  { 1, 0, 0 }
};

/* Interfaces */
//Just one interface
static
struct UsbInterface interfaces[1] = {
  { -1, 0, 1 }
 // { 1, 0, 1 }
};

/* String descriptor */
static
struct StringDescriptor descriptors[3] = {
   {18,
    USB_DT_STRING,
    { 'A', 'n', 'd', 'y', '_',  'm', 'a' , 'n',0,0,0,0,0,0,0,0}
   }, 
  {
    0,
    USB_DT_STRING
  }
};

/* HID descriptor */
static
unsigned char hiddesc_gamepad[] = {
  USB_DT_HID_SIZE, 
  USB_DT_HID, 
  0x10, 0x01, 
  0, 
  1, 
  USB_DT_REPORT, 
  sizeof(ReportDescriptorGamepad), 00 
};

/* HI-Speed device descriptor */
static
struct DeviceDescriptor devdesc_hi =
{
  USB_DT_DEVICE_SIZE,
  USB_DT_DEVICE,
  USB_BCD_200,         /* bcdUSB */
  USB_CLASS_PER_INTERFACE,  /* bDeviceClass */
  0,             /* bDeviceSubClass */
  0,             /* bDeviceProtocol */
  64,            /* bMaxPacketSize0 */
    0,             /* idVendor */
    0,             /* idProduct */
  0x103,         /* bcdDevice 0x290*/
    0,             /* iManufacturer */
    0,             /* iProduct */
    0,             /* iSerialNumber */
  1              /* bNumConfigurations */
};

/* Hi-Speed endpoint descriptors */
static
struct EndpointDescriptor endpdesc_hi[2] =
{
  {
    USB_DT_ENDPOINT_SIZE,
    USB_DT_ENDPOINT,
    0x81, /* bEndpointAddress */
    0x03, /* bmAttributes */
    0x03, /* wMaxPacketSize */
    0x0A  /* bInterval */
  },
  {
    0,
  }
};

/* Hi-Speed interface descriptor */
static
struct InterfaceDescriptor interdesc_hi[2] =
{
  {
    USB_DT_INTERFACE_SIZE,
    USB_DT_INTERFACE,
    0,      /* bInterfaceNumber */
    0,      /* bAlternateSetting */
    1,      /* bNumEndpoints */
    USB_CLASS_HID ,   /* bInterfaceClass */
    0x00,   /* bInterfaceSubClass */
    0x00,   /* bInterfaceProtocol */
    1,      /* iInterface */
    &endpdesc_hi[0], /* endpoints */
    hiddesc_gamepad,
    sizeof (hiddesc_gamepad)
  },
  {
    0
  }
};

/* Hi-Speed settings */
static
struct InterfaceSettings settings_hi[1] =
{
  {
    &interdesc_hi[0],
    0,
    1
  }
};

/* Hi-Speed configuration descriptor */
static
struct ConfigDescriptor confdesc_hi =
{
  USB_DT_CONFIG_SIZE,
  USB_DT_CONFIG,
  (USB_DT_INTERFACE_SIZE + USB_DT_CONFIG_SIZE + USB_DT_ENDPOINT_SIZE + USB_DT_HID_SIZE), /* wTotalLength */
  1,      /* bNumInterfaces */
  1,      /* bConfigurationValue */
  0,      /* iConfiguration */
  0xC0,   /* bmAttributes */
  0,      /* bMaxPower */
  &settings_hi[0]
};


/* Hi-Speed configuration */
static
struct UsbConfiguration config_hi =
{
  &confdesc_hi,
  &settings_hi[0],
  &interdesc_hi[0],
  &endpdesc_hi[0]
};

/* Full-Speed device descriptor */
static
struct DeviceDescriptor devdesc_full =
{
  USB_DT_DEVICE_SIZE,
  USB_DT_DEVICE,
  USB_BCD_110,         /* bcdUSB */
  USB_CLASS_PER_INTERFACE,  /* bDeviceClass */
  0,             /* bDeviceSubClass */
  0,             /* bDeviceProtocol */
  8,            /* bMaxPacketSize0 */
  	0,             /* idProduct */
  	0,             /* idVendor */
  0x103,         /* bcdDevice */
  	0,             /* iManufacturer */
 	0,             /* iProduct */
  	0,             /* iSerialNumber */
  1              /* bNumConfigurations */
};

/* Full-Speed endpoint descriptors */
static
struct EndpointDescriptor endpdesc_full[2] =
{
  {
    USB_DT_ENDPOINT_SIZE ,
    USB_DT_ENDPOINT,
    0x81, /* bEndpointAddress */
    0x03, /* bmAttributes */
    0x04, /* wMaxPacketSize */
    0x0A  /* bInterval */
  },
  {
    0,
  }
};


/* Full-Speed interface descriptor */
static
struct InterfaceDescriptor interdesc_full[2] =
{
  {
    USB_DT_INTERFACE_SIZE,
    USB_DT_INTERFACE,
    0,      /* bInterfaceNumber */
    0,      /* bAlternateSetting */
    1,      /* bNumEndpoints */
    USB_CLASS_HID,   /* bInterfaceClass */
    0x00,   /* bInterfaceSubClass */
    0x00,   /* bInterfaceProtocol */
    1,      /* iInterface */
    &endpdesc_full[0], /* endpoints */
    hiddesc_gamepad,
    sizeof (hiddesc_gamepad)
  },
  {
    0
  }
};


/* Full-Speed settings */
static
struct InterfaceSettings settings_full[1] =
{
  {
    &interdesc_full[0],
    0,
    1
  }
};

/* Full-Speed configuration descriptor */
static
struct ConfigDescriptor confdesc_full =
{
  USB_DT_CONFIG_SIZE,
  USB_DT_CONFIG,
  (USB_DT_INTERFACE_SIZE + USB_DT_CONFIG_SIZE + USB_DT_ENDPOINT_SIZE + USB_DT_HID_SIZE), /* wTotalLength */
  1,      /* bNumInterfaces */
  1,      /* bConfigurationValue */
  0,      /* iConfiguration */
  0xC0,   /* bmAttributes */
  0,      /* bMaxPower */
  &settings_full[0]
};

/* Full-Speed configuration */
static
struct UsbConfiguration config_full =
{
  &confdesc_full,
  &settings_full[0],
  &interdesc_full[0],
  &endpdesc_full[0]
};

/* Forward define driver functions */
static int start_func (int size, void *args);
static int stop_func (int size, void *args);
static int usb_recvctl (int arg1, int arg2, struct DeviceRequest *req);
static int usb_change (int interfaceNumber, int alternateSetting);
static int usb_attach (int usb_version);
static void usb_detach (void);
static void usb_configure (int usb_version, int desc_count, struct InterfaceSettings *settings);

/* USB host driver */
struct UsbDriver driver =
{
  PSP_USB_HID,        /* driverName */
  2,                           /* numEndpoints */
  &endpoints[0],               /* endpoints */
  &interfaces[0],              /* interface */
  &devdesc_hi,                 /* descriptor_hi */
  &config_hi,                  /* configuration_hi */
  &devdesc_full,               /* descriptor */
  &config_full,                /* configuration */
  &descriptors[0],             /* stringDescriptors */
  &usb_recvctl,                /* processRequest */
  &usb_change,                 /* chageSetting */
  &usb_attach,                 /* attach */
  &usb_detach,                 /* detach */
  &usb_configure,              /* configure */
  &start_func,                 /* start func */
  &stop_func,                  /* stop func */
  NULL                         /* link to driver */
};