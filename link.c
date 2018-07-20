#include <ctype.h>

#include "string.h"
#include "link.h"

static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    Link *mem = (Link *)userp;

    mem->body = realloc(mem->body, mem->body_sz + realsize + 1);
    if(mem->body == NULL) {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    memcpy(&(mem->body[mem->body_sz]), contents, realsize);
    mem->body_sz += realsize;
    mem->body[mem->body_sz] = 0;

    return realsize;
}

Link *Link_new(const char *p_url)
{
    Link *link = calloc(1, sizeof(Link));

    strncpy(link->p_url, p_url, LINK_LEN_MAX);

    link->type = LINK_UNKNOWN;
    link->curl = curl_easy_init();
    link->res = -1;

    /* set up some basic curl stuff */
    curl_easy_setopt(link->curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(link->curl, CURLOPT_WRITEDATA, (void *)link);
    curl_easy_setopt(link->curl, CURLOPT_USERAGENT, "mount-http-dir/libcurl");

    return link;
}

void Link_free(Link *link)
{
    curl_easy_cleanup(link->curl);
    free(link->body);
    free(link);
    link = NULL;
}

LinkTable *LinkTable_new(const char *url)
{
    LinkTable *linktbl = calloc(1, sizeof(LinkTable));

    /* populate the base URL */
    LinkTable_add(linktbl, Link_new(url));
    Link *head_link = linktbl->links[0];
    curl_easy_setopt(head_link->curl, CURLOPT_URL, url);

    /* start downloading the base URL */
    head_link->res = curl_easy_perform(head_link->curl);

    /* if downloading base URL failed */
    if (head_link->res != CURLE_OK) {
        fprintf(stderr, "link.c: LinkTable_new() cannot retrive the base URL");
        LinkTable_free(linktbl);
        linktbl = NULL;
        return linktbl;
    };

    /* Otherwise parsed the received data */
    GumboOutput* output = gumbo_parse(head_link->body);
    HTML_to_LinkTable(output->root, linktbl);
    gumbo_destroy_output(&kGumboDefaultOptions, output);

    /* Fill in the link table */
    LinkTable_fill(linktbl);
    return linktbl;
}

void LinkTable_free(LinkTable *linktbl)
{
    for (int i = 0; i < linktbl->num; i++) {
        Link_free(linktbl->links[i]);
    }
    free(linktbl->links);
    free(linktbl);
    linktbl = NULL;
}

void LinkTable_add(LinkTable *linktbl, Link *link)
{
    linktbl->num++;
    linktbl->links = realloc(
        linktbl->links,
        linktbl->num * sizeof(Link *));
    linktbl->links[linktbl->num - 1] = link;
}

void LinkTable_fill(LinkTable *linktbl)
{
    for (int i = 0; i < linktbl->num; i++) {
        Link *this_link = linktbl->links[i];
        if (this_link->type == LINK_UNKNOWN) {
            CURL *curl = this_link->curl;
            char *url;
            curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
            url = url_append(linktbl->links[0]->p_url, this_link->p_url);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            free(url);
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
            curl_easy_perform(curl);
            double cl;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);
            if (cl == -1) {
                this_link->content_length = 0;
                this_link->type = LINK_DIR;
            } else {
                this_link->content_length = cl;
                this_link->type = LINK_FILE;
            }
        }
    }
}


void LinkTable_print(LinkTable *linktbl)
{
    for (int i = 0; i < linktbl->num; i++) {
        Link *this_link = linktbl->links[i];
        printf("%d %c %lu %s\n",
               i,
               this_link->type,
               this_link->content_length,
               this_link->p_url);
    }
}

static int is_valid_link(const char *n)
{
    /* The link name has to start with alphanumerical character */
    if (!isalnum(n[0])) {
        return 0;
    }

    /* check for http:// and https:// */
    int c = strnlen(n, LINK_LEN_MAX);
    if (c > 5) {
        if (n[0] == 'h' && n[1] == 't' && n[2] == 't' && n[3] == 'p') {
            if ((n[4] == ':' && n[5] == '/' && n[6] == '/') ||
                (n[4] == 's' && n[5] == ':' && n[6] == '/' && n[7] == '/')) {
                return 0;
            }
        }
    }
    return 1;
}

/*
 * Shamelessly copied and pasted from:
 * https://github.com/google/gumbo-parser/blob/master/examples/find_links.cc
 */
void HTML_to_LinkTable(GumboNode *node, LinkTable *linktbl)
{
    if (node->type != GUMBO_NODE_ELEMENT) {
        return;
    }
    GumboAttribute* href;

    if (node->v.element.tag == GUMBO_TAG_A &&
        (href = gumbo_get_attribute(&node->v.element.attributes, "href"))) {
        /* if it is valid, copy the link onto the heap */
        if (is_valid_link(href->value)) {
            LinkTable_add(linktbl, Link_new(href->value));
        }
    }

    /* Note the recursive call, lol. */
    GumboVector *children = &node->v.element.children;
    for (size_t i = 0; i < children->length; ++i) {
        HTML_to_LinkTable((GumboNode*)children->data[i], linktbl);
    }
    return;
}

/* the upper level */
char *url_upper(const char *url)
{
    const char *pt = strrchr(url, '/');
    /* +1 for the '/' */
    size_t  len = pt - url + 1;
    char *str = strndup(url, len);
    str[len] = '\0';
    return str;
}

/* append url */
char *url_append(const char *url, const char *sublink)
{
    int needs_separator = 0;
    if (url[strlen(url)-1] != '/') {
        needs_separator = 1;
    }

    char *str;
    size_t ul = strlen(url);
    size_t sl = strlen(sublink);
    str = calloc(ul + sl + needs_separator, sizeof(char));
    strncpy(str, url, ul);
    if (needs_separator) {
        str[ul] = '/';
    }
    strncat(str, sublink, sl);
    return str;
}
