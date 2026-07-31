#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESPONSE_HEADER "SIMPLEBROWSE_WEBKITD_RESPONSE_V1"
#define RESPONSE_LIMIT (16u * 1024u * 1024u)

/*
 * WKWebView has no synchronous command-line API.  The daemon stays on the
 * AppKit thread and pumps its run loop while navigation and JavaScript
 * completion handlers do their work.  This preserves SimpleBrowse's existing
 * persistent-helper protocol without requiring a GUI application bundle.
 */
@interface SimpleBrowseWebKitDaemon : NSObject <WKNavigationDelegate>
@property(nonatomic, strong) WKWebView *view;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic) BOOL navigationDone;
@property(nonatomic) BOOL navigationFailed;
@property(nonatomic) NSUInteger navigationGeneration;
@property(nonatomic) NSInteger statusCode;
@property(nonatomic, copy) NSString *navigationError;
@property(nonatomic) NSTimeInterval timeoutSeconds;
@end

static const char snapshot_script[] =
"(() => {\n"
"  function clean(value) { return (value || '').replace(/\\s+/g, ' ').trim(); }\n"
"  function absolute(value) {\n"
"    if (!value) return '';\n"
"    try {\n"
"      const url = new URL(value, window.location.href);\n"
"      return /^(https?|file):$/.test(url.protocol) ? url.href : '';\n"
"    } catch (_) { return ''; }\n"
"  }\n"
"  function label(el) {\n"
"    return clean(el.getAttribute('aria-label') || el.innerText ||\n"
"      el.textContent || el.getAttribute('title') || el.value ||\n"
"      el.getAttribute('placeholder') || el.getAttribute('name') || el.id || '');\n"
"  }\n"
"  const sourceRoot = document.documentElement;\n"
"  if (!sourceRoot) return JSON.stringify({url: location.href, title: document.title, html: ''});\n"
"  const copyRoot = sourceRoot.cloneNode(true);\n"
"  const sources = [sourceRoot, ...sourceRoot.querySelectorAll('*')];\n"
"  const copies = [copyRoot, ...copyRoot.querySelectorAll('*')];\n"
"  const discard = new Set(['SCRIPT','STYLE','NOSCRIPT','TEMPLATE','SVG','CANVAS',\n"
"    'OBJECT','EMBED','SOURCE','TRACK','PORTAL','IFRAME']);\n"
"  for (let i = 0; i < sources.length && i < copies.length; i++) {\n"
"    const source = sources[i], copy = copies[i];\n"
"    if (!copy || !copy.parentNode && copy !== copyRoot) continue;\n"
"    const hiddenInput = source.tagName === 'INPUT' &&\n"
"      ((source.getAttribute('type') || source.type || '') + '').toLowerCase() === 'hidden';\n"
"    let hidden = !hiddenInput &&\n"
"      (source.hidden || source.getAttribute('aria-hidden') === 'true');\n"
"    if (!hidden && source !== sourceRoot) {\n"
"      const style = window.getComputedStyle(source);\n"
"      hidden = !!style && (style.display === 'none' || style.visibility === 'hidden' || Number(style.opacity) === 0);\n"
"    }\n"
"    if (discard.has(source.tagName) || hidden) { if (copy !== copyRoot) copy.remove(); continue; }\n"
"    for (const attr of ['href', 'action']) {\n"
"      if (source.hasAttribute && source.hasAttribute(attr)) {\n"
"        const value = absolute(source.getAttribute(attr));\n"
"        if (value) copy.setAttribute(attr, value); else copy.removeAttribute(attr);\n"
"      }\n"
"    }\n"
"    copy.removeAttribute && copy.removeAttribute('style');\n"
"    if (source.tagName === 'FORM')\n"
"      copy.setAttribute('data-simplebrowse-form-index', String(Array.from(document.forms || []).indexOf(source)));\n"
"    if (/^(INPUT|TEXTAREA|SELECT|BUTTON)$/.test(source.tagName)) {\n"
"      copy.setAttribute('data-simplebrowse-label', label(source) || 'Control');\n"
"      if ('value' in source) copy.setAttribute('value', source.value || '');\n"
"      if ('checked' in source) source.checked ? copy.setAttribute('checked', '') : copy.removeAttribute('checked');\n"
"      if (source.tagName === 'TEXTAREA') copy.textContent = source.value || '';\n"
"      if (source.tagName === 'SELECT') {\n"
"        const sourceOptions = source.options || [], copyOptions = copy.options || [];\n"
"        for (let n = 0; n < sourceOptions.length && n < copyOptions.length; n++)\n"
"          sourceOptions[n].selected ? copyOptions[n].setAttribute('selected', '') : copyOptions[n].removeAttribute('selected');\n"
"      }\n"
"    }\n"
"    if (!copy.closest('a') && /^(BUTTON)$/.test(source.tagName)) {\n"
"      const candidate = absolute(source.getAttribute('data-href') || source.getAttribute('data-url') || '');\n"
"      if (candidate && label(source)) {\n"
"        const anchor = document.createElement('a');\n"
"        anchor.href = candidate; anchor.textContent = label(source);\n"
"        copy.insertAdjacentElement('afterend', anchor);\n"
"      }\n"
"    }\n"
"  }\n"
"  let head = copyRoot.querySelector('head');\n"
"  if (!head) { head = document.createElement('head'); copyRoot.insertBefore(head, copyRoot.firstChild); }\n"
"  const base = document.createElement('base'); base.href = window.location.href; head.insertBefore(base, head.firstChild);\n"
"  return JSON.stringify({\n"
"    url: window.location.href,\n"
"    title: document.title || '',\n"
"    readyState: document.readyState || '',\n"
"    html: '<!doctype html>\\n' + copyRoot.outerHTML\n"
"  });\n"
"})()";

static const char submit_script_prefix[] =
"((payloadText) => {\n"
"  const payload = JSON.parse(payloadText);\n"
"  const forms = Array.from(document.forms || []);\n"
"  function score(form) {\n"
"    if (!form) return -1;\n"
"    let total = 0;\n"
"    const elements = Array.from(form.elements || []);\n"
"    for (const control of payload.controls || []) {\n"
"      if (!control.name) continue;\n"
"      for (const el of elements) if (el.name === control.name) total += control.active ? 20 : 5;\n"
"    }\n"
"    if (form.querySelector(\"input[type='search'],input[name='q']\")) total += 25;\n"
"    return total;\n"
"  }\n"
"  let form = forms[payload.form_index] || forms[0] || null;\n"
"  for (const candidate of forms) if (score(candidate) > score(form)) form = candidate;\n"
"  if (!form) return 'SIMPLEBROWSE_ERROR no form';\n"
"  let active = null;\n"
"  for (const control of payload.controls || []) {\n"
"    const elements = Array.from(form.elements || []).filter(el => el.name === control.name);\n"
"    for (const el of elements) {\n"
"      const tag = (el.tagName || '').toLowerCase();\n"
"      const type = ((el.getAttribute('type') || el.type || '') + '').toLowerCase();\n"
"      if (type === 'radio') el.checked = !!control.checked && (!control.value || el.value === control.value);\n"
"      else if (type === 'checkbox') el.checked = !!control.checked;\n"
"      else if (tag === 'select') {\n"
"        el.value = control.value || '';\n"
"        if (typeof control.selected === 'number') el.selectedIndex = control.selected;\n"
"      } else if (type !== 'submit' && type !== 'button') el.value = control.value || '';\n"
"      el.dispatchEvent(new Event('input', {bubbles: true}));\n"
"      el.dispatchEvent(new Event('change', {bubbles: true}));\n"
"      if (control.active) active = el;\n"
"    }\n"
"  }\n"
"  if ((form.method || 'get').toLowerCase() === 'get') {\n"
"    const params = new URLSearchParams();\n"
"    for (const el of Array.from(form.elements || [])) {\n"
"      const type = ((el.getAttribute('type') || el.type || '') + '').toLowerCase();\n"
"      if (type === 'hidden' && el.name && !el.disabled) params.append(el.name, el.value || '');\n"
"    }\n"
"    for (const control of payload.controls || [])\n"
"      if (control.name && (control.active || control.value)) params.set(control.name, control.value || '');\n"
"    let target = new URL(form.action || window.location.href, window.location.href);\n"
"    if (target.hostname.endsWith('duckduckgo.com') && params.get('q'))\n"
"      target = new URL('https://html.duckduckgo.com/html/');\n"
"    target.search = params.toString();\n"
"    return 'SIMPLEBROWSE_NAVIGATE ' + target.toString();\n"
"  }\n"
"  let submitter = null;\n"
"  if (payload.submit_name) submitter = Array.from(form.elements || []).find(el =>\n"
"    el.name === payload.submit_name && (!payload.submit_value || el.value === payload.submit_value) &&\n"
"    ((el.getAttribute('type') || el.type || '') + '').toLowerCase() === 'submit');\n"
"  if (!submitter) submitter = Array.from(form.elements || []).find(el =>\n"
"    ((el.getAttribute('type') || el.type || '') + '').toLowerCase() === 'submit');\n"
"  if (active) active.focus();\n"
"  if (form.requestSubmit) form.requestSubmit(submitter || undefined);\n"
"  else if (form.dispatchEvent(new Event('submit', {bubbles: true, cancelable: true}))) form.submit();\n"
"  return 'submitted';\n"
"})(";

static void pump_run_loop(NSTimeInterval seconds)
{
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];

    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
}

@implementation SimpleBrowseWebKitDaemon

- (instancetype)init
{
    self = [super init];
    if (self) {
        WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
        NSString *timeout = NSProcessInfo.processInfo.environment[@"SIMPLEBROWSE_JS_TIMEOUT_MS"];
        NSInteger timeoutMS = timeout.integerValue;

        if (timeoutMS < 1000)
            timeoutMS = 20000;
        self.timeoutSeconds = (NSTimeInterval)timeoutMS / 1000.0;
        configuration.websiteDataStore = WKWebsiteDataStore.defaultDataStore;
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = NO;
        self.view = [[WKWebView alloc]
            initWithFrame:NSMakeRect(0, 0, 1280, 900)
            configuration:configuration];
        self.view.navigationDelegate = self;
        NSString *userAgent =
            NSProcessInfo.processInfo.environment[@"SIMPLEBROWSE_WEBKIT_UA"];
        if (userAgent.length)
            self.view.customUserAgent = userAgent;

        self.window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(-12000, -12000, 1280, 900)
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        self.window.contentView = self.view;
        self.window.alphaValue = 0.01;
        [self.window orderFront:nil];
    }
    return self;
}

- (void)webView:(WKWebView *)webView
    didStartProvisionalNavigation:(WKNavigation *)navigation
{
    (void)webView;
    (void)navigation;
    self.navigationGeneration++;
    self.navigationDone = NO;
    self.navigationFailed = NO;
    self.navigationError = @"";
    self.statusCode = 0;
}

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse *)navigationResponse
    decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler
{
    (void)webView;
    if ([navigationResponse.response isKindOfClass:NSHTTPURLResponse.class])
        self.statusCode =
            ((NSHTTPURLResponse *)navigationResponse.response).statusCode;
    decisionHandler(WKNavigationResponsePolicyAllow);
}

- (void)webView:(WKWebView *)webView
    didFinishNavigation:(WKNavigation *)navigation
{
    (void)webView;
    (void)navigation;
    self.navigationDone = YES;
}

- (void)recordNavigationFailure:(NSError *)error
{
    self.navigationFailed = YES;
    self.navigationDone = YES;
    self.navigationError = error.localizedDescription ?: @"WebKit load failed";
}

- (void)webView:(WKWebView *)webView
    didFailNavigation:(WKNavigation *)navigation
    withError:(NSError *)error
{
    (void)webView;
    (void)navigation;
    [self recordNavigationFailure:error];
}

- (void)webView:(WKWebView *)webView
    didFailProvisionalNavigation:(WKNavigation *)navigation
    withError:(NSError *)error
{
    (void)webView;
    (void)navigation;
    [self recordNavigationFailure:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView
{
    (void)webView;
    self.navigationFailed = YES;
    self.navigationDone = YES;
    self.navigationError = @"WebKit content process terminated";
}

- (BOOL)waitForNavigation:(NSString **)error
{
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:self.timeoutSeconds];

    while (!self.navigationDone && deadline.timeIntervalSinceNow > 0)
        pump_run_loop(MIN(0.05, deadline.timeIntervalSinceNow));
    if (!self.navigationDone) {
        [self.view stopLoading];
        if (error)
            *error = @"timed out waiting for WebKit page";
        return NO;
    }
    if (self.navigationFailed) {
        if (error)
            *error = self.navigationError;
        return NO;
    }
    return YES;
}

- (BOOL)loadURL:(NSString *)url error:(NSString **)error
{
    NSURL *target = [NSURL URLWithString:url];

    if (!target) {
        if (error)
            *error = @"invalid URL";
        return NO;
    }
    self.navigationDone = NO;
    self.navigationFailed = NO;
    [self.view loadRequest:[NSURLRequest requestWithURL:target]];
    return [self waitForNavigation:error];
}

- (id)evaluate:(NSString *)script error:(NSString **)error
{
    __block BOOL done = NO;
    __block id value = nil;
    __block NSError *evaluationError = nil;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:self.timeoutSeconds];

    [self.view evaluateJavaScript:script completionHandler:^(id result,
                                                             NSError *failure) {
        value = result;
        evaluationError = failure;
        done = YES;
    }];
    while (!done && deadline.timeIntervalSinceNow > 0)
        pump_run_loop(MIN(0.05, deadline.timeIntervalSinceNow));
    if (!done) {
        if (error)
            *error = @"timed out evaluating page JavaScript";
        return nil;
    }
    if (evaluationError) {
        if (error)
            *error = evaluationError.localizedDescription;
        return nil;
    }
    return value;
}

- (NSDictionary *)snapshot:(NSString **)error
{
    id value = [self evaluate:[NSString stringWithUTF8String:snapshot_script]
                        error:error];
    if (![value isKindOfClass:NSString.class]) {
        if (error && !*error)
            *error = @"DOM extraction returned no text";
        return nil;
    }
    NSData *data = [(NSString *)value dataUsingEncoding:NSUTF8StringEncoding];
    id decoded = [NSJSONSerialization JSONObjectWithData:data
                                                  options:0
                                                    error:nil];
    if (![decoded isKindOfClass:NSDictionary.class]) {
        if (error)
            *error = @"DOM extraction returned an invalid response";
        return nil;
    }
    NSDictionary *snapshot = decoded;
    NSString *html = snapshot[@"html"];

    if (![html isKindOfClass:NSString.class] || !html.length) {
        if (error)
            *error = @"post-JavaScript DOM was empty";
        return nil;
    }
    if ([html lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > RESPONSE_LIMIT) {
        if (error)
            *error = @"post-JavaScript DOM exceeded the response limit";
        return nil;
    }
    return snapshot;
}

- (BOOL)submitPayload:(NSString *)payload error:(NSString **)error
{
    NSData *quotedData =
        [NSJSONSerialization dataWithJSONObject:@[payload ?: @""] options:0 error:nil];
    NSString *quotedArray =
        [[NSString alloc] initWithData:quotedData encoding:NSUTF8StringEncoding];
    NSString *quoted = [quotedArray substringWithRange:
        NSMakeRange(1, quotedArray.length - 2)];
    NSString *script = [NSString stringWithFormat:@"%s%@)", submit_script_prefix,
                                                  quoted];
    NSUInteger generation = self.navigationGeneration;
    id result = [self evaluate:script error:error];

    if (!result)
        return NO;
    if ([result isKindOfClass:NSString.class] &&
        [(NSString *)result hasPrefix:@"SIMPLEBROWSE_ERROR "]) {
        if (error)
            *error = [(NSString *)result substringFromIndex:19];
        return NO;
    }
    if ([result isKindOfClass:NSString.class] &&
        [(NSString *)result hasPrefix:@"SIMPLEBROWSE_NAVIGATE "]) {
        return [self loadURL:[(NSString *)result substringFromIndex:22]
                       error:error];
    }

    /*
     * requestSubmit() may start navigation asynchronously, while script-owned
     * forms may only redraw the current DOM.  Give either path a short settle
     * window, then wait for a navigation if one began.
     */
    NSDate *settle = [NSDate dateWithTimeIntervalSinceNow:0.25];
    while (settle.timeIntervalSinceNow > 0)
        pump_run_loop(MIN(0.025, settle.timeIntervalSinceNow));
    if (self.navigationGeneration != generation && !self.navigationDone)
        return [self waitForNavigation:error];
    if (self.navigationFailed) {
        if (error)
            *error = self.navigationError;
        return NO;
    }
    return YES;
}

@end

static NSString *read_payload(size_t length)
{
    unsigned char *bytes;
    size_t used = 0;
    int trail;

    if (length > RESPONSE_LIMIT)
        return nil;
    bytes = malloc(length ? length : 1);
    if (!bytes)
        return nil;
    while (used < length) {
        size_t count = fread(bytes + used, 1, length - used, stdin);

        if (count == 0) {
            free(bytes);
            return nil;
        }
        used += count;
    }
    trail = fgetc(stdin);
    if (trail != '\n') {
        free(bytes);
        return nil;
    }
    NSString *text = [[NSString alloc] initWithBytes:bytes
                                              length:length
                                            encoding:NSUTF8StringEncoding];
    free(bytes);
    return text ?: @"";
}

static void emit_response(NSString *status, NSInteger code, NSString *url,
                          NSString *title, NSString *error, NSString *html)
{
    NSData *urlData = [(url ?: @"") dataUsingEncoding:NSUTF8StringEncoding];
    NSData *titleData = [(title ?: @"") dataUsingEncoding:NSUTF8StringEncoding];
    NSData *errorData = [(error ?: @"") dataUsingEncoding:NSUTF8StringEncoding];
    NSData *htmlData = [(html ?: @"") dataUsingEncoding:NSUTF8StringEncoding];

    fprintf(stdout, RESPONSE_HEADER "\n");
    fprintf(stdout, "STATUS %s\n", status.UTF8String);
    fprintf(stdout, "CODE %ld\n", (long)code);
    fprintf(stdout, "URL_LENGTH %zu\n", (size_t)urlData.length);
    fprintf(stdout, "TITLE_LENGTH %zu\n", (size_t)titleData.length);
    fprintf(stdout, "ERROR_LENGTH %zu\n", (size_t)errorData.length);
    fprintf(stdout, "HTML_LENGTH %zu\n\n", (size_t)htmlData.length);
    fwrite(urlData.bytes, 1, urlData.length, stdout);
    fwrite(titleData.bytes, 1, titleData.length, stdout);
    fwrite(errorData.bytes, 1, errorData.length, stdout);
    fwrite(htmlData.bytes, 1, htmlData.length, stdout);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        char *line = NULL;
        size_t capacity = 0;

        if (argc == 2 && !strcmp(argv[1], "--version")) {
            puts("simplebrowse-webkitd 4.0.0 (WKWebView)");
            return 0;
        }
        if (argc != 1) {
            fprintf(stderr, "usage: simplebrowse-webkitd [--version]\n");
            return 2;
        }
        signal(SIGPIPE, SIG_IGN);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        SimpleBrowseWebKitDaemon *daemon =
            [[SimpleBrowseWebKitDaemon alloc] init];

        while (getline(&line, &capacity, stdin) >= 0) {
            @autoreleasepool {
                size_t urlLength = 0;
                size_t payloadLength = 0;
                NSString *url;
                NSString *payload = nil;
                NSString *error = nil;
                BOOL submit = NO;
                BOOL loaded;

                line[strcspn(line, "\r\n")] = '\0';
                if (!strcmp(line, "QUIT"))
                    break;
                if (sscanf(line, "LOAD %zu", &urlLength) == 1) {
                    submit = NO;
                } else if (sscanf(line, "SUBMIT %zu %zu",
                                  &urlLength, &payloadLength) == 2) {
                    submit = YES;
                } else {
                    emit_response(@"ERROR", 2, @"", @"",
                                  @"unknown command", @"");
                    continue;
                }
                url = read_payload(urlLength);
                if (submit)
                    payload = read_payload(payloadLength);
                if (!url || (submit && !payload)) {
                    emit_response(@"ERROR", 1, @"", @"",
                                  @"truncated command payload", @"");
                    continue;
                }

                loaded = submit &&
                    [daemon.view.URL.absoluteString isEqualToString:url];
                if (!loaded)
                    loaded = [daemon loadURL:url error:&error];
                if (loaded && submit)
                    loaded = [daemon submitPayload:payload error:&error];
                if (!loaded) {
                    emit_response(@"ERROR", daemon.statusCode,
                                  daemon.view.URL.absoluteString ?: url,
                                  daemon.view.title ?: @"", error, @"");
                    continue;
                }
                NSDictionary *snapshot = [daemon snapshot:&error];
                if (!snapshot) {
                    emit_response(@"ERROR", daemon.statusCode,
                                  daemon.view.URL.absoluteString ?: url,
                                  daemon.view.title ?: @"", error, @"");
                    continue;
                }
                NSInteger code = daemon.statusCode > 0
                                   ? daemon.statusCode : 200;
                emit_response(@"OK", code,
                              snapshot[@"url"] ?: daemon.view.URL.absoluteString,
                              snapshot[@"title"] ?: daemon.view.title,
                              @"", snapshot[@"html"]);
            }
        }
        free(line);
        [daemon.window close];
    }
    return 0;
}
