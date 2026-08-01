#import <CoreWLAN/CoreWLAN.h>
#import <CoreLocation/CoreLocation.h>
#import <Security/Security.h>
#import <SecurityFoundation/SFAuthorization.h>

#include <stdio.h>
#include <string.h>

#include "simplenet-macos.h"

@interface SimpleNetLocationDelegate : NSObject <CLLocationManagerDelegate>
@end

@implementation SimpleNetLocationDelegate
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager
{
    (void)manager;
}
@end

static void copy_nsstring(char *destination, size_t size, NSString *source)
{
    const char *text;

    if (!destination || size == 0)
        return;
    text = source.UTF8String;
    snprintf(destination, size, "%s", text ? text : "");
}

static void set_error(char *destination, size_t size, NSString *message)
{
    copy_nsstring(destination, size, message);
}

static CWInterface *default_interface(void)
{
    return CWWiFiClient.sharedWiFiClient.interface;
}

static int location_authorized(char *error, size_t error_size)
{
    CLLocationManager *manager;
    SimpleNetLocationDelegate *delegate;
    CLAuthorizationStatus status;

    if (![CLLocationManager locationServicesEnabled]) {
        set_error(error, error_size,
                  @"Location Services are disabled; enable them for Wi-Fi identifiers.");
        return 0;
    }
    manager = [[CLLocationManager alloc] init];
    delegate = [[SimpleNetLocationDelegate alloc] init];
    manager.delegate = delegate;
    status = manager.authorizationStatus;
    if (status == kCLAuthorizationStatusNotDetermined) {
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:30.0];

        [manager requestWhenInUseAuthorization];
        do {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            status = manager.authorizationStatus;
        } while (status == kCLAuthorizationStatusNotDetermined &&
                 deadline.timeIntervalSinceNow > 0);
    }
    manager.delegate = nil;
    [delegate release];
    [manager release];
    if (status == kCLAuthorizationStatusAuthorizedAlways ||
        status == kCLAuthorizationStatusAuthorizedWhenInUse)
        return 1;
    if (status == kCLAuthorizationStatusDenied ||
        status == kCLAuthorizationStatusRestricted)
        set_error(error, error_size,
                  @"Allow Location Services for SimpleNet (or its terminal) in System Settings.");
    else
        set_error(error, error_size,
                  @"Location authorization is still pending; scan again after responding.");
    return 0;
}

static int signal_percent(NSInteger rssi)
{
    NSInteger percent = ((rssi + 90) * 100 + 30) / 60;

    if (percent < 0)
        return 0;
    if (percent > 100)
        return 100;
    return (int)percent;
}

static int network_is_enterprise(CWNetwork *network)
{
    return [network supportsSecurity:kCWSecurityWPAEnterprise] ||
           [network supportsSecurity:kCWSecurityWPAEnterpriseMixed] ||
           [network supportsSecurity:kCWSecurityWPA2Enterprise] ||
           [network supportsSecurity:kCWSecurityWPA3Enterprise] ||
           [network supportsSecurity:kCWSecurityEnterprise] ||
           [network supportsSecurity:kCWSecurityDynamicWEP];
}

static int network_is_secured(CWNetwork *network)
{
    return ![network supportsSecurity:kCWSecurityNone];
}

static NSString *network_security(CWNetwork *network)
{
    if ([network supportsSecurity:kCWSecurityWPA3Enterprise])
        return @"WPA3 Enterprise";
    if ([network supportsSecurity:kCWSecurityWPA3Personal])
        return @"WPA3";
    if ([network supportsSecurity:kCWSecurityWPA3Transition])
        return @"WPA2/3";
    if ([network supportsSecurity:kCWSecurityWPA2Enterprise] ||
        [network supportsSecurity:kCWSecurityWPAEnterpriseMixed] ||
        [network supportsSecurity:kCWSecurityEnterprise])
        return @"WPA2 Enterprise";
    if ([network supportsSecurity:kCWSecurityWPA2Personal] ||
        [network supportsSecurity:kCWSecurityWPAPersonalMixed] ||
        [network supportsSecurity:kCWSecurityPersonal])
        return @"WPA2";
    if ([network supportsSecurity:kCWSecurityWPAEnterprise])
        return @"WPA Enterprise";
    if ([network supportsSecurity:kCWSecurityWPAPersonal])
        return @"WPA";
    if ([network supportsSecurity:kCWSecurityDynamicWEP])
        return @"802.1X/WEP";
    if ([network supportsSecurity:kCWSecurityWEP])
        return @"WEP";
    if ([network supportsSecurity:kCWSecurityNone])
        return @"open";
    return @"unknown";
}

int simplenet_macos_interface(char *name, size_t name_size,
                              char *ssid, size_t ssid_size,
                              char *bssid, size_t bssid_size,
                              int *powered)
{
    @autoreleasepool {
        CWInterface *interface = default_interface();

        if (name && name_size)
            name[0] = '\0';
        if (ssid && ssid_size)
            ssid[0] = '\0';
        if (bssid && bssid_size)
            bssid[0] = '\0';
        if (powered)
            *powered = 0;
        if (!interface)
            return 0;
        copy_nsstring(name, name_size, interface.interfaceName);
        copy_nsstring(ssid, ssid_size, interface.ssid);
        copy_nsstring(bssid, bssid_size, interface.bssid);
        if (powered)
            *powered = interface.powerOn ? 1 : 0;
        return 1;
    }
}

int simplenet_macos_scan(SimpleNetMacAccessPoint *points, int maximum,
                         char *error, size_t error_size)
{
    @autoreleasepool {
        CWInterface *interface = default_interface();
        NSError *scan_error = nil;
        NSSet<CWNetwork *> *networks;
        NSString *active_ssid;
        NSString *active_bssid;
        int count = 0;

        if (error && error_size)
            error[0] = '\0';
        if (!points || maximum <= 0) {
            set_error(error, error_size, @"Invalid scan buffer.");
            return -1;
        }
        if (!interface) {
            set_error(error, error_size, @"No CoreWLAN interface is available.");
            return -1;
        }
        if (!location_authorized(error, error_size))
            return -1;
        if (!interface.powerOn) {
            set_error(error, error_size, @"Wi-Fi is turned off.");
            return -1;
        }
        networks = [interface scanForNetworksWithName:nil error:&scan_error];
        if (!networks) {
            set_error(error, error_size,
                      scan_error.localizedDescription ?: @"CoreWLAN scan failed.");
            return -1;
        }
        active_ssid = interface.ssid;
        active_bssid = interface.bssid;
        for (CWNetwork *network in networks) {
            SimpleNetMacAccessPoint *point;

            if (count >= maximum)
                break;
            if (!network.ssid.length || !network.bssid.length)
                continue;
            point = &points[count++];
            memset(point, 0, sizeof(*point));
            copy_nsstring(point->ssid, sizeof(point->ssid), network.ssid);
            copy_nsstring(point->bssid, sizeof(point->bssid), network.bssid);
            point->channel = (int)network.wlanChannel.channelNumber;
            point->signal = signal_percent(network.rssiValue);
            point->active =
                active_ssid.length && active_bssid.length &&
                [network.ssid isEqualToString:active_ssid] &&
                [network.bssid caseInsensitiveCompare:active_bssid] ==
                    NSOrderedSame;
            point->secured = network_is_secured(network);
            point->enterprise = network_is_enterprise(network);
            copy_nsstring(point->security, sizeof(point->security),
                          network_security(network));
        }
        if (count == 0) {
            set_error(error, error_size,
                      @"No named networks were returned. Allow Location Services "
                       "for the terminal running SimpleNet, then scan again.");
        }
        return count;
    }
}

static CWNetwork *find_network(CWInterface *interface, NSString *ssid,
                               NSString *bssid, NSError **error)
{
    NSSet<CWNetwork *> *networks =
        [interface scanForNetworksWithName:ssid error:error];

    if (!networks)
        return nil;
    for (CWNetwork *network in networks) {
        if (!bssid.length ||
            (network.bssid.length &&
             [network.bssid caseInsensitiveCompare:bssid] == NSOrderedSame))
            return network;
    }
    return nil;
}

static NSString *saved_password(CWNetwork *network)
{
    NSString *password = nil;

    if (!network.ssidData)
        return nil;
    if (CWKeychainFindWiFiPassword(kCWKeychainDomainUser,
                                   network.ssidData, &password) == errSecSuccess)
        return password;
    password = nil;
    if (CWKeychainFindWiFiPassword(kCWKeychainDomainSystem,
                                   network.ssidData, &password) == errSecSuccess)
        return password;
    return nil;
}

static CWSecurity preferred_security(CWNetwork *network)
{
    if ([network supportsSecurity:kCWSecurityWPA3Personal])
        return kCWSecurityWPA3Personal;
    if ([network supportsSecurity:kCWSecurityWPA3Transition])
        return kCWSecurityWPA3Transition;
    if ([network supportsSecurity:kCWSecurityWPA2Personal])
        return kCWSecurityWPA2Personal;
    if ([network supportsSecurity:kCWSecurityWPAPersonalMixed])
        return kCWSecurityWPAPersonalMixed;
    if ([network supportsSecurity:kCWSecurityWPAPersonal])
        return kCWSecurityWPAPersonal;
    if ([network supportsSecurity:kCWSecurityWEP])
        return kCWSecurityWEP;
    return kCWSecurityNone;
}

static int prefer_network(CWInterface *interface, CWNetwork *network,
                          char *error, size_t error_size)
{
    CWConfiguration *current;
    CWMutableConfiguration *configuration;
    NSMutableOrderedSet *profiles;
    CWNetworkProfile *selected_profile = nil;
    NSError *operation_error = nil;
    SFAuthorization *authorization;
    BOOL committed;

    if (!interface || !network.ssidData) {
        set_error(error, error_size,
                  @"The selected network has no persistent profile identity.");
        return 0;
    }
    current = interface.configuration;
    if (!current) {
        set_error(error, error_size,
                  @"macOS did not provide the Wi-Fi configuration.");
        return 0;
    }
    configuration = [current mutableCopy];
    profiles = [configuration.networkProfiles mutableCopy];
    if (!profiles)
        profiles = [[NSMutableOrderedSet alloc] init];
    for (CWNetworkProfile *profile in profiles) {
        if (profile.ssidData &&
            [profile.ssidData isEqualToData:network.ssidData]) {
            selected_profile = [profile retain];
            break;
        }
    }
    if (!selected_profile) {
        CWMutableNetworkProfile *profile =
            [[CWMutableNetworkProfile alloc] init];

        profile.ssidData = network.ssidData;
        profile.security = preferred_security(network);
        selected_profile = profile;
    }
    if (profiles.count > 0 &&
        [[[profiles objectAtIndex:0] ssidData]
            isEqualToData:selected_profile.ssidData] &&
        configuration.rememberJoinedNetworks) {
        [selected_profile release];
        [profiles release];
        [configuration release];
        return 1;
    }
    [profiles removeObject:selected_profile];
    [profiles insertObject:selected_profile atIndex:0];
    configuration.networkProfiles = profiles;
    configuration.rememberJoinedNetworks = YES;
    authorization = [SFAuthorization authorization];
    committed = [interface commitConfiguration:configuration
                                     authorization:authorization
                                             error:&operation_error];
    [authorization invalidateCredentials];
    [selected_profile release];
    [profiles release];
    [configuration release];
    if (!committed) {
        set_error(error, error_size,
                  operation_error.localizedDescription ?:
                  @"macOS did not save the preferred Wi-Fi network.");
        return 0;
    }
    return 1;
}

int simplenet_macos_prefer_network(const char *ssid_text,
                                   const char *bssid_text,
                                   char *error, size_t error_size)
{
    @autoreleasepool {
        CWInterface *interface = default_interface();
        NSString *ssid =
            ssid_text ? [NSString stringWithUTF8String:ssid_text] : nil;
        NSString *bssid =
            bssid_text ? [NSString stringWithUTF8String:bssid_text] : nil;
        NSError *scan_error = nil;
        CWNetwork *network;

        if (error && error_size)
            error[0] = '\0';
        if (!interface || !ssid.length) {
            set_error(error, error_size,
                      @"No Wi-Fi interface or SSID is available.");
            return 0;
        }
        if (!location_authorized(error, error_size))
            return 0;
        network = find_network(interface, ssid, bssid, &scan_error);
        if (!network) {
            set_error(error, error_size,
                      scan_error.localizedDescription ?:
                      @"The selected network is no longer visible.");
            return 0;
        }
        return prefer_network(interface, network, error, error_size);
    }
}

int simplenet_macos_connect(const char *ssid_text, const char *bssid_text,
                            const char *password_text, int use_saved_password,
                            char *error, size_t error_size)
{
    @autoreleasepool {
        CWInterface *interface = default_interface();
        NSString *ssid = ssid_text ? [NSString stringWithUTF8String:ssid_text] : nil;
        NSString *bssid =
            bssid_text ? [NSString stringWithUTF8String:bssid_text] : nil;
        NSString *password = nil;
        CWNetwork *network;
        NSError *operation_error = nil;
        BOOL associated;

        if (error && error_size)
            error[0] = '\0';
        if (!interface || !ssid.length) {
            set_error(error, error_size, @"No Wi-Fi interface or SSID is available.");
            return SIMPLENET_MACOS_CONNECT_FAILED;
        }
        if (!location_authorized(error, error_size))
            return SIMPLENET_MACOS_CONNECT_FAILED;
        network = find_network(interface, ssid, bssid, &operation_error);
        if (!network) {
            set_error(error, error_size,
                      operation_error.localizedDescription ?:
                      @"The selected mesh node is no longer visible.");
            return SIMPLENET_MACOS_CONNECT_FAILED;
        }
        if (network_is_enterprise(network)) {
            set_error(error, error_size,
                      @"Enterprise Wi-Fi association remains managed by macOS.");
            return SIMPLENET_MACOS_ENTERPRISE_UNSUPPORTED;
        }
        if (network_is_secured(network)) {
            if (password_text && password_text[0])
                password = [NSString stringWithUTF8String:password_text];
            else if (use_saved_password)
                password = saved_password(network);
            if (!password.length)
                return SIMPLENET_MACOS_PASSWORD_REQUIRED;
        }
        associated = [interface associateToNetwork:network
                                          password:password
                                             error:&operation_error];
        if (!associated) {
            set_error(error, error_size,
                      operation_error.localizedDescription ?: @"Association failed.");
            return SIMPLENET_MACOS_CONNECT_FAILED;
        }
        if (password_text && password_text[0] && network.ssidData)
            (void)CWKeychainSetWiFiPassword(kCWKeychainDomainUser,
                                            network.ssidData, password);
        if (!prefer_network(interface, network, error, error_size))
            return SIMPLENET_MACOS_CONNECTED_NOT_SAVED;
        return SIMPLENET_MACOS_CONNECT_OK;
    }
}
