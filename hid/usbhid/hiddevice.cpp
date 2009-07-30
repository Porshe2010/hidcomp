// hidcomp/hidconfig, HID device interface for emc
// Copyright (C) 2008, Frank Tkalcevic, www.franksworkshop.com

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "common.h"

#ifdef _WIN32
#pragma warning(push, 1)
#endif

#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#ifdef _WIN32
#pragma warning(pop)
#pragma warning(disable:4251)
#endif

#include "hiddevice.h"
#include "hidparser.h"
#ifdef _WIN32
#include "usb.h"
#else
#include "libusb.h"
#endif
#include "hidtypes.h"
#include "log.h"

#include <assert.h>

#define GET_REPORT	1
#define THREAD_WAIT_TIME	25	// msec


#ifdef _WIN32
    #define TIMEOUT_ERROR	-116
#else
    #define TIMEOUT_ERROR	LIBUSB_ERROR_TIMEOUT

    /* HID descriptor */
    struct usb_hid_descriptor {
	    u_int8_t  bLength;
	    u_int8_t  bDescriptorType;
	    u_int16_t bcdHID;
	    u_int8_t  bCountryCode;
	    u_int8_t  bNumDescriptors;
	    /* u_int8_t  bReportDescriptorType; */
	    /* u_int16_t wDescriptorLength; */
	    /* ... */
    };

    #define USB_DT_REPORT LIBUSB_DT_REPORT
    #define USB_TIMEOUT 1000
    #define USB_ENDPOINT_OUT    LIBUSB_ENDPOINT_OUT
    #define USB_ENDPOINT_IN	LIBUSB_ENDPOINT_IN
    #define USB_ENDPOINT_TYPE_INTERRUPT	LIBUSB_TRANSFER_TYPE_INTERRUPT
#endif



#ifdef _WIN32
HIDDevice::HIDDevice(struct usb_device *dev, byte nInterface, byte nConfig )
#else
HIDDevice::HIDDevice(struct libusb_device *dev, byte nInterface, byte nConfig )
#endif
: m_Logger(QCoreApplication::applicationName(),"HIDDevice")
, m_dev(dev)
, m_hDev( NULL )
, m_nInterface( nInterface )
, m_nConfig( nConfig )
, m_bOpen( false )
, m_bOpenErrors( false )
, m_nClaimed( -1 )
#ifdef HAS_ASYNC
, m_bRunning( false )
, m_bAsyncStarted( false )
, m_pThread( NULL )
#endif
{
#ifndef _WIN32
    if ( m_dev != NULL )
	libusb_ref_device( m_dev );
#endif
}

HIDDevice::~HIDDevice(void)
{
#ifdef HAS_ASYNC
    StopThread();
#endif
#ifndef _WIN32
    if ( m_dev != NULL )
	libusb_unref_device( m_dev );
#endif
    m_dev = NULL;
}


bool HIDDevice::PreprocessReportData()
{
    bool bOpened = false;
    if ( !m_bOpen )
    {
	// if we open the device, we will close it.
	bOpened = true;
	if ( !Open() )
	    return false;
    }

#ifndef _WIN32
    Claim();
#endif
    bool bRet = true;

    byte buffer[256];	// Max HID Report buffer size - length stored in a byte
    memset( buffer, 0, sizeof(buffer) );

#ifdef _WIN32
    int len = usb_control_msg( m_hDev, USB_ENDPOINT_IN+1, USB_REQ_GET_DESCRIPTOR, (USB_DT_HID << 8) + 0, 0, (char*)buffer, sizeof(buffer), USB_TIMEOUT );
#else
    //int len = libusb_get_descriptor( m_hDev, LIBUSB_DT_HID, 0, buffer, sizeof(buffer) );
    //int len = libusb_control_transfer( m_hDev, LIBUSB_ENDPOINT_IN+1, LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_HID << 8) + 0, 0, buffer, sizeof(buffer), USB_TIMEOUT );
    int len = libusb_control_transfer( m_hDev, LIBUSB_ENDPOINT_IN+1, LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_HID << 8) + 0, 0, buffer, sizeof(buffer), USB_TIMEOUT );

#endif
    HID_DESCRIPTOR *desc = (HID_DESCRIPTOR *)buffer;
    if ( desc->bLength != len )
    {
        LOG_MSG( m_Logger, LogTypes::Error, QString("Error reading HID descriptor.  desc->bLength != len (%1!=%2)").arg(desc->bLength).arg(len) );
	bRet = false;
    }
    else
    {
	for ( int i = 0; i < desc->bNumDescriptors; i++ )
	{
	    if ( desc->Descriptor[i].bDescriptorType == USB_DT_REPORT )
	    {
		unsigned short nReportLen = desc->Descriptor[i].wDescriptorLength;
		QVector<byte> pReportDesc( nReportLen );

#ifdef _WIN32
		len = usb_control_msg( m_hDev, USB_ENDPOINT_IN+1, USB_REQ_GET_DESCRIPTOR, (USB_DT_REPORT << 8) + desc->Descriptor[i].bDescriptorType, 0, (char *)pReportDesc.data(), nReportLen, USB_TIMEOUT );
#else
		len = libusb_control_transfer( m_hDev, LIBUSB_ENDPOINT_IN+1, LIBUSB_REQUEST_GET_DESCRIPTOR, (USB_DT_REPORT << 8) + desc->Descriptor[i].bDescriptorType, 0, pReportDesc.data(), nReportLen, USB_TIMEOUT );
#endif

		if ( len != nReportLen )
		{
		    LOG_MSG( m_Logger, LogTypes::Error, QString("Error reading HID report descriptor %1.  len != nReportLen (%2!=%3)").arg(i).arg(len).arg(nReportLen) );
		    bRet = false;
		}
		else
                {
		    HIDParser parser;
                    parser.ParseReportData( pReportDesc.constData(), nReportLen, m_ReportInfo );
                }

		// There can only be 1 REPORT DESCRIPTOR
		break;
	    }
	}
    }

#ifndef _WIN32
    Unclaim();
#endif

    // Only close the device if we opened it.
    if ( bOpened )
	Close();

    return bRet;
}


// Look up a descriptor string.  We cache them as we read them.
QString HIDDevice::GetDescriptorString( unsigned char index )
{
    // A string index must be > 0
    QString s;
    if ( index <= 0 )
	return s;

    // Check the cache first.
    std::map<byte,QString>::iterator it = m_DescriptorStrings.find( index );
    if ( it != m_DescriptorStrings.end() )
	return it->second;

    // Nope.  Get it from the device.
    bool bOpened = false;
    if ( !m_bOpen )
    {
	// if we open the device, we will close it.
	bOpened = true;
	if ( !Open() )
	    return s;
    }

    // The length of a USB string, in bytes, is stored in a byte, so 256 is the max len (really (256-2)/2 -> packet header and data is Unicode)
    byte buf[256];
#ifdef _WIN32
    int n = usb_get_string_simple( m_hDev, index, (char *)buf, sizeof(buf) );
#else
    int n = libusb_get_string_descriptor_ascii( m_hDev, index, buf, sizeof(buf) );
#endif
    if ( n < 0 )
    {
	LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to retrive string at index %1: %2").arg(index).arg(n) );
    }
    else
        s = QString::fromLatin1((const char *)buf, n);

    // trim whitespace
    s = s.trimmed();

    // Cache successful fetches
    if ( n >= 0 )
	m_DescriptorStrings[index] = s;

    // Only close the device if we opened it.
    if ( bOpened )
	Close();

    return s;
}



// Retreive the current configuration
int HIDDevice::GetConfiguration()
{
    //bool bOpened = false;
    //if ( !m_bOpen )
    //{
    //	// if we open the device, we will close it.
    //	bOpened = true;
    //	if ( !Open() )
    //		return -1;
    //}

    //   byte const HID_CONFURATION_LEN = 1;
    //   byte buffer[HID_CONFURATION_LEN];

    //int nRet;
    //   int len = usb_control_msg( m_hDev, USB_ENDPOINT_IN, USB_REQ_GET_CONFIGURATION, 0, 0, (char*)buffer, sizeof(buffer), USB_TIMEOUT );
    //   if ( len != sizeof(buffer) )
    //   {
    //       char *sError = usb_strerror();
    //	LOG_ERROR( "Error reading Configuration state.  len != 1(%d!=1): %s\n", len, sError );
    //       nRet = -1;
    //   }
    //else
    //	nRet = *buffer;

    //if ( bOpened )
    //	Close();

    //   return nRet;
    return 0;
}


bool HIDDevice::Open()
{
    LOG_MSG( m_Logger, LogTypes::Debug, "Opening USB device" );

    if ( m_bOpen )
    {
	LOG_MSG( m_Logger, LogTypes::Debug, "Already open" );
	return true;
    }

#ifdef _WIN32
    m_hDev = usb_open(m_dev);
    if ( m_hDev == NULL )
    {
        char *sError = usb_strerror();
	LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to open usb device %1: '%2'").arg(m_dev->filename).arg(sError) );
	m_bOpenErrors = true;
	return false;
    }
#else
    m_hDev = NULL;
    int nError = libusb_open(m_dev, &m_hDev);
    if ( nError != 0 )
    {
	LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to open usb device: %1").arg(nError) );
	m_bOpenErrors = true;
	return false;
    }
#endif

    LOG_MSG( m_Logger, LogTypes::Debug, "USB device successfully openned" );

    m_bOpen = true;
    return true;
}

bool HIDDevice::Claim()
{
    LOG_MSG( m_Logger, LogTypes::Debug, "Claiming USB Device" );

    assert( m_bOpen );
    if ( !m_bOpen )
    {
	LOG_MSG( m_Logger, LogTypes::Error, QString("Claim::device not open"));
	return false;
    }

    if ( m_nClaimed == 0 )
    {
	LOG_MSG( m_Logger, LogTypes::Debug, "Already claimed USB Device" );
	return true;
    }

    // The behaviour of libusb and libusb-win32 seems to be different.  libusb-win32 wants the
    // configuration set first.  Even if get configuration returns the required configuration
    // we still need to set it.  lib-usb, on the other hand, only wants the configuration set
    // if it is different and after we've claimed the interface.
#ifdef _WIN32
    m_nInterface = m_dev->config->_interface->altsetting->bInterfaceNumber;
    LOG_MSG( m_Logger, LogTypes::Debug, QString("Claiming interface '%1': '%2'").arg(m_dev->filename).arg(m_nInterface) );
    m_nClaimed = usb_claim_interface( m_hDev, m_nInterface );
    LOG_MSG( m_Logger, LogTypes::Debug, QString("usb_claim_interface returned: %1").arg(m_nClaimed) );
    if ( m_nClaimed == -22 )
    {
	int configuration = m_dev->config->bConfigurationValue;
        LOG_MSG( m_Logger, LogTypes::Debug, QString("Setting configuration: %1").arg(configuration) );
	int ret = usb_set_configuration(m_hDev, configuration);
	if ( ret < 0 )
	{
	    char *sError = usb_strerror();
	    LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to set device configuration %1: '%2'").arg(configuration).arg(sError) );
	    m_bOpenErrors = true;
	    return false;
	}
        LOG_MSG( m_Logger, LogTypes::Debug, QString("Claiming interface again '%1': '%2'").arg(m_dev->filename).arg(m_nInterface) );
	m_nClaimed = usb_claim_interface( m_hDev, m_nInterface );
        LOG_MSG( m_Logger, LogTypes::Debug, QString("usb_claim_interface returned: %1").arg(m_nClaimed) );
    }
    if ( m_nClaimed != 0 )
    {
	char *sError = usb_strerror();
	LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to claim interface '%1': '%2'").arg(m_dev->filename).arg(sError) );
	//   int configuration = m_dev->config->bConfigurationValue;
	//   int ret = usb_set_configuration(m_hDev, configuration);
	//   if ( ret < 0 )
	//{
	//	char *sError = usb_strerror();
	//	LOG_WARNING("Failed to set device configuration %d: '%s'\n", configuration, sError );
	m_bOpenErrors = true;
	return false;
    }
#else

    LOG_MSG( m_Logger, LogTypes::Debug, QString("Class config=%1, interface=%2").arg(m_nConfig).arg(m_nInterface) );
    
    int nError;
 //   libusb_config_descriptor *config;
 //   int nError = libusb_get_config_descriptor_by_value( m_dev, configuration, &config );
 //   if ( nError != 0 )
 //   {
	//LOG_MSG( m_Logger, LogTypes::Error, QString("Error reading config descriptor %1: %2").arg(configuration).arg(nError) );
 //   }
 //   else
 //   {
	//m_nInterface = config->interface->altsetting->bInterfaceNumber;	// is this right?  Which interface do we use?
	//libusb_free_config_descriptor( config );
 //   }

    int nCurrentConfig = -1;
    nError = libusb_get_configuration( m_hDev, &nCurrentConfig );
    if ( nError != 0 )
    {
	LOG_MSG( m_Logger, LogTypes::Error, QString("Error reading Configuration state: %1").arg(nError) );
    }
    LOG_MSG( m_Logger, LogTypes::Debug, QString("Current configuration %1").arg(nCurrentConfig) );

    if ( m_nConfig != nCurrentConfig )
    {
        LOG_MSG( m_Logger, LogTypes::Debug, QString("Setting configuration to %1").arg(m_nConfig) );
	nError = libusb_set_configuration(m_hDev, m_nConfig);
	if ( nError != 0 )
	{
	    LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to set device configuration %1: '%2'").arg(m_nConfig).arg(nError) );
	    m_bOpenErrors = true;
	    return false;
	}
    }

    m_nClaimed = libusb_claim_interface( m_hDev, m_nInterface );
    if ( m_nClaimed != 0 )
    {
	LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to claim interface '%1': '%2'").arg(m_nInterface).arg(m_nClaimed) );

	nError = libusb_kernel_driver_active( m_hDev, m_nInterface );
	if ( nError == 1 )
	{
	    LOG_MSG( m_Logger, LogTypes::Debug, "Kernel driver active.  Trying to force claim." );
	    nError = libusb_detach_kernel_driver(m_hDev, m_nInterface);
	    if ( nError != 0 )
	    {
		LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to force detach kernel driver %1: '%2'").arg(m_nInterface).arg(nError) );
		m_bOpenErrors = true;
		return false;
	    }
	    LOG_MSG( m_Logger, LogTypes::Debug, QString("Kernel driver detached.  Claiming interface %1").arg(m_nInterface) );
	    m_nClaimed = libusb_claim_interface( m_hDev, m_nInterface );
	    if ( m_nClaimed != 0 )
	    {
		LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to claim interface after detaching kernel driver '%1': '%2'").arg(m_nInterface).arg(m_nClaimed) );
		m_bOpenErrors = true;
		return false;
	    }
	}
	else
	{
	    LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to determine if the kernel driver is active for configuration %1: '%2'").arg(m_nConfig).arg(nError) );
	    m_bOpenErrors = true;
	    return false;
	}
    }
    else
    {
        LOG_MSG( m_Logger, LogTypes::Debug, QString("Claimed interface %1").arg(m_nInterface) );
    }
#endif

    LOG_MSG( m_Logger, LogTypes::Debug, "Device claimed" );

    m_bOpenErrors = false;
    m_bOpen = true;
    return true;
}

bool HIDDevice::Unclaim()
{
    LOG_MSG( m_Logger, LogTypes::Debug, "Unclaiming device" );

    if ( m_nClaimed == 0 )
    {
#ifdef _WIN32
	int nError = usb_release_interface( m_hDev,  m_nInterface );
#else
	int nError = libusb_release_interface( m_hDev,  m_nInterface );
#endif
	if ( nError < 0 )
	{
	    LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to release interface: '%1'").arg(nError) );
	}
	m_nClaimed = -1;
    }

    return true;
}


bool HIDDevice::Close()
{
    assert( m_nClaimed != 0 );
    LOG_MSG( m_Logger, LogTypes::Debug, "Closing device" );

    if ( m_bOpen )
    {
#ifdef _WIN32
	int nError = usb_close(m_hDev);
	if ( nError < 0 )
	{
	    LOG_MSG( m_Logger, LogTypes::Warning, QString("Failed to close usb device: '%1'").arg(nError) );
	}
#else
	libusb_close(m_hDev);
#endif
	m_hDev = NULL;
	m_bOpen = false;
    }

    return true;
}




QString HIDDevice::Manufacturer() 
{ 
#ifdef _WIN32
    return GetDescriptorString( m_dev->descriptor.iProduct );
#else
    QString s;
    libusb_device_descriptor desc;
    if ( libusb_get_device_descriptor( m_dev,  &desc ) == 0 )
	s = GetDescriptorString( desc.iManufacturer ); 
    return s;
#endif
} 

QString HIDDevice::Product()
{ 
#ifdef _WIN32
    return GetDescriptorString( m_dev->descriptor.iProduct );
#else
    QString s;
    libusb_device_descriptor desc;
    if ( libusb_get_device_descriptor( m_dev,  &desc ) == 0 )
	s = GetDescriptorString( desc.iProduct ); 
    return s;
#endif
} 

QString HIDDevice::SerialNumber()
{ 
#ifdef _WIN32
    return GetDescriptorString( m_dev->descriptor.iSerialNumber );
#else
    QString s;
    libusb_device_descriptor desc;
    if ( libusb_get_device_descriptor( m_dev,  &desc ) == 0 )
	s = GetDescriptorString( desc.iSerialNumber ); 
    return s;
#endif
} 

unsigned short HIDDevice::VID()
{ 
#ifdef _WIN32
    return m_dev->descriptor.idVendor;
#else
    libusb_device_descriptor desc;
    if ( libusb_get_device_descriptor( m_dev,  &desc ) == 0 )
	return desc.idVendor;
    else
	return 0;
#endif
} 

unsigned short HIDDevice::PID()
{ 
#ifdef _WIN32
    return m_dev->descriptor.idProduct;
#else
    libusb_device_descriptor desc;
    if ( libusb_get_device_descriptor( m_dev,  &desc ) == 0 )
	return desc.idProduct;
    else
	return 0;
#endif
} 

QString HIDDevice::SystemId() 
{ 
    if ( m_sSystemId.isEmpty() )
    {
	#ifdef _WIN32
	    m_sSystemId = QString( "%1" ).arg( m_dev->filename );
	#else
	    // for linux, every time the device is removed, it comes back with another devnum, the only thing libusb gives us.
	    // So, we go looking for a /sys/bus/usb/devices/*/devnum that matches our devnum and use the * part to uniquely identify our bus position

	    LOG_MSG( m_Logger, LogTypes::Debug, QString("Bus %1 Address %2").arg(libusb_get_bus_number(m_dev)).arg(libusb_get_device_address(m_dev)) );
	    m_sSystemId = QString("?");
	    int nDeviceNumber = libusb_get_device_address(m_dev);

	    QDir devicesPath("/sys/bus/usb/devices");
	    QFileInfoList devices = devicesPath.entryInfoList( QDir::Dirs );
	    for ( int i = 0; i < devices.count(); i++ )
	    {
		QFileInfo devnumFile( devices[i].filePath() + "/devnum" );
		if ( devnumFile.exists() )
		{
		    QFile file(devnumFile.filePath());
		    if ( file.open( QIODevice::ReadOnly |  QIODevice::Text ) )
		    {
			QTextStream in(&file);
			QString line = in.readLine();
			file.close();

			if ( line.toInt() == nDeviceNumber )
			{
			    m_sSystemId = devices[i].fileName();
			    break;
			}
		    }
		}
	    }
	#endif
    }
    return m_sSystemId; 
}

int HIDDevice::InterruptRead( byte *buf, int len, int timeout )
{
    bool bOpened = false;
    if ( !m_bOpen )
    {
	// if we open the device, we will close it.
	bOpened = true;
	if ( !Open() )
	    return -1;
    }

#ifdef _WIN32
    int n = usb_interrupt_read( m_hDev, InputEndpoint(), (char *)buf, len, timeout );
#else
    int transferred = 0;
    int n = libusb_interrupt_transfer( m_hDev, InputEndpoint(), buf, len, &transferred, timeout );
    if ( n == 0 )
	n = transferred;
#endif

    if ( bOpened )
	Close();

    return n;
}

bool HIDDevice::GetReport( byte nReportId, HID_ReportItemTypes_t nReportType, byte *buf, int nBufLen, int timeout )
{
    bool bOpened = false;
    if ( !m_bOpen )
    {
	// if we open the device, we will close it.
	bOpened = true;
	if ( !Open() )
	    return false;
    }

    bool bRet = true;

#ifdef _WIN32
    int len = usb_control_msg( m_hDev, 0xA1, GET_REPORT, ((nReportType+1) << 8) + nReportId, 0, (char*)buf, nBufLen, timeout );
#else
    int len = libusb_control_transfer( m_hDev, 0xA1, GET_REPORT, ((nReportType+1) << 8) + nReportId, 0, buf, nBufLen, timeout );
#endif
    if ( nBufLen != len )
    {
	LOG_MSG( m_Logger, LogTypes::Error, QString( "Error requesting report %1: %2\n").arg(nReportId).arg(len) );
	bRet = false;
    }

    // Only close the device if we opened it.
    if ( bOpened )
	Close();

    return bRet;
}


int HIDDevice::InterruptWrite( const byte *buf, int len, int timeout )
{
    bool bOpened = false;
    if ( !m_bOpen )
    {
	// if we open the device, we will close it.
	bOpened = true;
	if ( !Open() )
	    return -1;
    }

#ifdef _WIN32
    int n = usb_interrupt_write( m_hDev, OutputEndpoint(), (char *)buf, len, timeout );
#else
    int transferred = 0;
    int n = libusb_interrupt_transfer( m_hDev, OutputEndpoint(), const_cast<byte *>(buf), len, &transferred, timeout );
    if ( n == 0 )
	n = transferred;
#endif

    if ( bOpened )
	Close();

    return n;
}



byte HIDDevice::OutputEndpoint()
{
    return GetEndpoint(USB_ENDPOINT_OUT, USB_ENDPOINT_TYPE_INTERRUPT);
}

byte HIDDevice::InputEndpoint()
{
    return GetEndpoint(USB_ENDPOINT_IN, USB_ENDPOINT_TYPE_INTERRUPT);
}


byte HIDDevice::GetEndpoint( byte nDirection, byte nType )
{
    // search for the correct end-point to output on.
#ifdef _WIN32
    assert( m_dev != NULL );
    assert( m_dev->config->bNumInterfaces == 1 );

    if ( m_dev != NULL && m_dev->config->bNumInterfaces == 1 )
    {
	if ( m_dev->config->_interface->num_altsetting > 0 )
	{
	    for ( int ep = 0; ep < m_dev->config->_interface->altsetting->bNumEndpoints; ep++ )
	    {
		if ( (m_dev->config->_interface->altsetting->endpoint[ep].bEndpointAddress & USB_ENDPOINT_DIR_MASK) == nDirection &&
		    (m_dev->config->_interface->altsetting->endpoint[ep].bmAttributes & USB_ENDPOINT_TYPE_MASK) == nType )
		{
		    return m_dev->config->_interface->altsetting->endpoint[ep].bEndpointAddress;
		}
	    }
	}
    }
#else
    libusb_config_descriptor *config_desc = NULL;

    if ( libusb_get_active_config_descriptor( m_dev, &config_desc ) == 0 )
    {
	for ( int i = 0; i < config_desc->bNumInterfaces; i++ )
	    for ( int a = 0; a <  config_desc->interface[i].num_altsetting; a++ )
		for ( int e = 0; e <  config_desc->interface[i].altsetting[a].bNumEndpoints; e++ )
		    if ( (config_desc->interface[i].altsetting[a].endpoint[e].bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == nDirection &&
		         (config_desc->interface[i].altsetting[a].endpoint[e].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == nType ) 
		    {
			byte ep =  config_desc->interface[i].altsetting[a].endpoint[e].bEndpointAddress;
			libusb_free_config_descriptor( config_desc );
			return ep;
		    }

	libusb_free_config_descriptor( config_desc );
    }
#endif

    return nDirection;	// default to ep 0
}


HID_ReportItem_t *HIDDevice::FindReportItem( HID_ReportItemTypes_t type, unsigned short nUsagePage, unsigned short nUsage )
{
    for (unsigned int i = 0; i < m_ReportInfo.ReportItems.size(); i++ )
    {
	HID_ReportItem_t *pItem = m_ReportInfo.ReportItems[i];
	if ( pItem->ItemType == type && pItem->Attributes.UsagePage == nUsagePage && pItem->Attributes.Usage == nUsage )
	    return pItem;
    }
    return NULL;
}

#ifdef HAS_ASYNC


bool HIDDevice::StartAsync()
{
    if ( m_bAsyncStarted )
    {
        LOG_MSG( m_Logger, LogTypes::Error, "USB Device Async thread already started" );
	return false;
    }

    // Find the largest input report so we can allocate a buffer to receive it.
    int nLongestInReport = 0;
    for ( std::map<byte, HID_ReportDetails_t>::iterator it = m_ReportInfo.Reports.begin(); it != m_ReportInfo.Reports.end(); it++ )
	if ( it->second.InReportLength > nLongestInReport )
	    nLongestInReport = it->second.InReportLength;

    // When there is more than one report (in,out or feature), the report id is inserted before the packet.
    if ( m_ReportInfo.Reports.size() > 1 )
	nLongestInReport++;

    // Find the largest output report so we can allocate a buffer to receive it.
    int nLongestOutReport = 0;
    for ( std::map<byte, HID_ReportDetails_t>::iterator it = m_ReportInfo.Reports.begin(); it != m_ReportInfo.Reports.end(); it++ )
	if ( it->second.OutReportLength > nLongestOutReport )
	    nLongestOutReport = it->second.OutReportLength;

    // When there is more than one report (in,out or feature), the report id is inserted before the packet.
    if ( m_ReportInfo.Reports.size() > 1 )
	nLongestOutReport++;

    m_pThread = new HIDDeviceThread( m_SendBufferMutex, m_SendBuffer, m_ReceiveBufferMutex, m_ReceiveBuffer, nLongestInReport, nLongestOutReport, this );
    m_pThread->start();	// QThread::HighPriority );  // Set priority higher than caller

    m_bAsyncStarted = true;
    return true;
}


void HIDDevice::StopAsync()
{
    LOG_MSG( m_Logger, LogTypes::Debug, "Received request to stop" );
    m_bRunning = false;
    StopThread();
}

void HIDDevice::StopThread()
{
    if ( m_pThread != NULL )
    {
	m_pThread->Stop();
	m_pThread->wait( THREAD_WAIT_TIME );
	delete m_pThread;
	m_pThread = NULL;
    }
}

//void HIDDevice::run()
//{
//    m_bRunning = true;
//
//    try
//    {
//	// Find the largest input report so we can allocate a buffer to receive it.
//	int nLongestReport = 0;
//	for ( std::map<byte, HID_ReportDetails_t>::iterator it = m_ReportInfo.Reports.begin(); it != m_ReportInfo.Reports.end(); it++ )
//	    if ( it->second.InReportLength > nLongestReport )
//		nLongestReport = it->second.InReportLength;
//
//	// When there is more than one report (in,out or feature), the report id is inserted before the packet.
//	if ( m_ReportInfo.Reports.size() > 1 )
//	    nLongestReport++;
//
//        const int MAX_CONSECUTIVE_ERRORS = 10;
//        int nErrorCount = 0;
//        QVector<byte> buf(nLongestReport);
//
//        while ( m_bRunning )
//        {
//            const unsigned long LOOP_TIMEOUT_MS = 10;
//
//	    // Read
//	    int n = InterruptRead( buf.data(), buf.size(), LOOP_TIMEOUT_MS );
//
//	    if ( n == TIMEOUT_ERROR )
//	    {
//	        // Nothing to do.
//                nErrorCount = 0;
//	    }
//	    else if ( n < 0 )
//	    {
//	        char *sError = usb_strerror();
//	        LOG_MSG( m_Logger, LogTypes::Debug, QString("Got error: %1: %2").arg(n).arg(sError) );
//                nErrorCount++;
//                if ( nErrorCount > MAX_CONSECUTIVE_ERRORS )
//                {
//                    LOG_MSG( m_Logger, LogTypes::Error, QString("Received %1 consecutive errors.  Terminating.").arg(nErrorCount) );
//                    throw;
//                }
//	    }
//	    else
//	    {
//	        LOG_MSG( m_Logger, LogTypes::Debug, QString( "Got %1 bytes\n").arg(n) );
//                if ( m_Logger.WillLog( LogTypes::Debug ) )
//                {
//	            QString  s;
//	            for ( int i = 0; i < n; i++ )
//	            {
//                        QString sNum;
//                        sNum.setNum( buf[i], 16 );
//		        s += sNum;
//	            }
//	            LOG_MSG( m_Logger, LogTypes::Debug, s );
//                }
//
//		{
//		    QMutexLocker lock(&m_ReceiveBufferMutex);
//		    m_ReceiveBuffer.push_back( buf.mid(0,n) );
//		}
//                nErrorCount = 0;
//	    }
//
//            // Anything to be sent
//	    while ( !m_SendBuffer.isEmpty() )
//	    {
//		QVector<byte> msg;
//
//		{
//		    QMutexLocker lock(&m_SendBufferMutex);
//		    if ( !m_SendBuffer.isEmpty() )
//			msg = m_SendBuffer.takeFirst();
//		}
//
//		if ( msg.count() > 0 )
//		    InterruptWrite( msg.data(), msg.count(), USB_TIMEOUT );
//	    }
//        }
//    }
//    catch (...)
//    {
//    }
//
//    m_bRunning = false;
//}


void HIDDevice::_WriteCallback(struct libusb_transfer *transfer)
{
    HIDDevice *pThis = static_cast<HIDDevice *>( transfer->user_data );
    pThis->WriteCallback( transfer );
}

void HIDDevice::WriteCallback(struct libusb_transfer *transfer)
{
    LOG_MSG( m_Logger, LogTypes::Debug, QString("Got write callback status=%1, transfered=%2").arg(transfer->status).arg(transfer->actual_length) );
    // todo clean up
}


int HIDDevice::AsyncInterruptWrite( const byte *buf, int len )
{
    if ( m_Logger.WillLog( LogTypes::Debug ) )
    {
	QString  s;
	for ( int i = 0; i < len; i++ )
	    s += QString("%1 ").arg(buf[i],2,16,QChar('0'));
        LOG_MSG( m_Logger, LogTypes::Debug, QString("Async Interrupt Write %1 bytes: %2").arg(len).arg(s) );
    }

    QVector<byte> msg(len);
    memcpy( msg.data(), buf, len );

    {
	QMutexLocker lock(&m_SendBufferMutex);
	m_SendBuffer.push_back( msg );
    }
    m_pThread->Signal();


 //   LOG_MSG( m_Logger, LogTypes::Debug, "Creating write buffer" );

 //   libusb_transfer *pTransfer = libusb_alloc_transfer(0);
 //   if ( pTransfer == NULL )
 //   {
	//LOG_MSG( m_Logger, LogTypes::Error, "Failed to allocate interrupt write packet" );
 //   }
 //   else
 //   {
	//byte *send_buf = new byte[len];
	//memcpy( send_buf, buf, len );
	//libusb_fill_interrupt_transfer( pTransfer, m_hDev, OutputEndpoint(), send_buf, len, &_WriteCallback, 0, 1000 );
	//int n = libusb_submit_transfer( pTransfer );
	//if ( n != 0 )
	//    LOG_MSG( m_Logger, LogTypes::Error, QString("Failed to submit write transfer packet %1" ).arg(n) );
 //   }

    return len;
}

int HIDDevice::AsyncInterruptRead( byte *buf, int len )
{
    if ( !m_ReceiveBuffer.isEmpty() )
    {
	QVector<byte> msg;
	{
	    QMutexLocker lock(&m_ReceiveBufferMutex);
	    if ( !m_ReceiveBuffer.isEmpty() )
		msg = m_ReceiveBuffer.takeFirst();
	}

	int nMsgLen = msg.count();
	if ( len < nMsgLen )
	    nMsgLen = len;
	memcpy( buf, msg.data(), nMsgLen );
	return nMsgLen;
    }
    return 0;
}

bool HIDDevice::RequestReport( byte nReportId, HID_ReportItemTypes_t nReportType, int nBufLen, int timeout )
{
    QVector<byte> buf( nBufLen );
    if ( GetReport( nReportId, nReportType, buf.data(), buf.count(), timeout ) )
    {
	QMutexLocker lock(&m_ReceiveBufferMutex);
	m_ReceiveBuffer.push_back( buf );
	return true;
    }
    else
	return false;
}


#endif
