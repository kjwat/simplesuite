#ifndef SIMPLEBROWSE_DOCUMENT_H
#define SIMPLEBROWSE_DOCUMENT_H

#include <stddef.h>

typedef struct {
    char *label;
    char *url;
    size_t offset;
} SimpleBrowseDocumentLink;

typedef struct {
    char *text;
    SimpleBrowseDocumentLink *links;
    size_t link_count;
} SimpleBrowseDocument;

int simplebrowse_document_from_html(const char *html, size_t length,
                                    const char *base_url,
                                    SimpleBrowseDocument *document);
void simplebrowse_document_free(SimpleBrowseDocument *document);

#endif
