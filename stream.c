#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#define BLOCK_SIZE (2048 * 1024)
#define BOUNDARY_SIZE 48

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#ifdef WIN32
#define strncasecmp _strnicmp
// #define strncpy strncpy_s
// #define vsprintf vspprintf_s
#endif

struct _XmlBlock {
  char block[BLOCK_SIZE];
  char boundary[BOUNDARY_SIZE];
  char *start;
  char *end;
  size_t length;
  size_t boundaryLength;
  char in_body;
};

typedef struct _XmlBlock XmlBlock;

void HandleXmlChunk(const char *xml);

/* Find a string in another string doing a case insensitive search */
char *strcasestr(const char *s, const char *f)
{
  const char *sp = s;
  const char *fp = f;
  
  while (*sp != '\0')
  {
    if (tolower(*sp) == tolower(*fp))
      fp++;
    else
      fp = f;
    if (*fp == '\0')
      return (char*) (sp - strlen(f));
    sp++;
  }
  
  return NULL;
}


void StreamXMLErrorFunc(void *ctx ATTRIBUTE_UNUSED, const char *msg, ...)
{
  va_list args;
  char buffer[2048];
  va_start(args, msg);
  vsnprintf(buffer, 2046, msg, args);
  buffer[2047] = '0';
  va_end(args);
  
  fprintf(stderr, "XML Error: %s\n", buffer);   
}

size_t HandleHeader(char *ptr, size_t size, size_t nmemb, void *data)
{
  char line[512];
  XmlBlock *block = (XmlBlock*) data;
  
  int len = size * nmemb;
  if (len > 511) len = 511;
  memcpy(line, ptr, len);
  line[len] = '\0';
  
  /* All we care about is the content type. */
  if (strncasecmp(line, "content-type:", 12) == 0)
  {
    char *bp;
    
    if (strstr(line, "multipart/x-mixed-replace") == NULL)
    {
      fprintf(stderr, "Incorrect content type: '%s', must be multipart/x-mixed-replace\n",
              line);
      exit(1);
    }
    
    bp = strstr(ptr, "boundary=");
    if (bp == NULL)
    {
      fprintf(stderr, "Cannot find boundary in %s\n", line);
      exit(1);
    }
    
    block->boundary[0] = block->boundary[1] = '-';
    strncpy(block->boundary + 2, bp + 9, BOUNDARY_SIZE - 3);
    block->boundary[BOUNDARY_SIZE - 1] = '\0';
    bp = block->boundary + (strlen(block->boundary) - 1);
    while (!isalnum(*bp))
      *bp-- = '\0';
    
    block->boundaryLength = strlen(block->boundary);
    
    printf("Found boundary: %s\n", block->boundary);
  }
  return size * nmemb;
}

size_t HandleData(char *ptr, size_t size, size_t nmemb, void *data)
{
  /* First find the boundary in the current block. */
  XmlBlock *block = (XmlBlock*) data;
  int need_data;
  
  if (block->boundary[0] == '\0')
  {
    fprintf(stderr, "Data arrived without boundary\n");
    exit(1);
  }

  /* Check for buffer overflow. returning a smaller number will cause this 
     to error out */
  if (size * nmemb > BLOCK_SIZE - (block->end - block->block))
    return BLOCK_SIZE - (block->end - block->block);
  
  /* append the new data to the end of the block and null terminate */
  memcpy(block->end, ptr, size * nmemb);
  block->end += size * nmemb;
  *(block->end) = '\0';
  
  do
  {
    need_data = TRUE;
    if (!block->in_body)
    {
      // Look for the boundary
      char *bp = strstr(block->start, block->boundary);
      if (bp != NULL)
      {
        char *ep = strstr(bp, "\r\n\r\n");
        if (ep != NULL)
        {
          // Parse the headers after the boundary for the content length.
          char *cp;
          bp += block->boundaryLength + 2;
          cp = strcasestr(bp, "Content-length:");
          if (cp != NULL) {
            block->length = atoi(cp + 16);
          }
          
          // Scan for the "\r\n\r\n"
          block->in_body = 1;
          block->start = ep + 4;
        }
      }
    }
    
    if (block->in_body && (block->end - block->start) >= block->length)
    {
      char *ep;
      size_t len;

      *(block->start + block->length) = '\0';
      
      /* We have a new chunk of xml data... */
      HandleXmlChunk(block->start);
      
      /* Consume the block and reset the pointers. */
      ep = block->start + block->length;
      len = block->end - ep;
      if (len > 0) memcpy(block->block, ep, len);
      block->start = block->block;
      block->end = block->block + len;
      block->length = 0;
      block->in_body = 0;
      
      if (len > 60) need_data = FALSE;
    }
  } while (!need_data);
  
  return size * nmemb;
}

int main(int argc, char *argv[])
{
  XmlBlock data;
  CURL *handle = curl_easy_init();
  
  memset(&data, 0, sizeof(data));
  data.start = data.end = data.block;
  
  //curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
  curl_easy_setopt(handle, CURLOPT_URL, argv[1]);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, HandleData);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &data);
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, HandleHeader);
  curl_easy_setopt(handle, CURLOPT_WRITEHEADER, &data);
  
  xmlInitParser();
  xmlXPathInit();
  xmlSetGenericErrorFunc(NULL, StreamXMLErrorFunc);

  curl_easy_perform(handle);
  
  return 0;
}


void HandleXmlChunk(const char *xml)
{
  xmlDocPtr document;
  int i;
  char *path;
  xmlXPathContextPtr xpathCtx;
  xmlNodePtr root;
  xmlXPathObjectPtr nodes;
  xmlNodeSetPtr nodeset;

  document = xmlReadDoc(BAD_CAST xml, "file://node.xml",
                        NULL, XML_PARSE_NOBLANKS);
  if (document == NULL) 
  {
    fprintf(stderr, "Cannot parse document: %s\n", xml);
    xmlFreeDoc(document);
    return;
  }
  
  path = "//m:Events/*|//m:Samples/*|//m:Condition/*";
  xpathCtx = xmlXPathNewContext(document);
  
  root = xmlDocGetRootElement(document);
  if (root->ns != NULL)
  {
    xmlXPathRegisterNs(xpathCtx, BAD_CAST "m", root->ns->href);
  }
  else
  {
    fprintf(stderr, "Document does not have a namespace: %s\n", xml);
    xmlFreeDoc(document);
    return;
  }
  
  // Evaluate the xpath.
  nodes = xmlXPathEval(BAD_CAST path, xpathCtx);
  if (nodes == NULL || nodes->nodesetval == NULL)
  {
    printf("No nodes found matching XPath\n");
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(document);
   return;
  }
  
  // Spin through all the events, samples and conditions.
  nodeset = nodes->nodesetval;
  for (i = 0; i != nodeset->nodeNr; ++i)
  {
    xmlNodePtr n = nodeset->nodeTab[i];
    xmlChar *name = xmlGetProp(n, BAD_CAST "name");
    xmlChar *value;

    if (name == NULL)
      name = xmlGetProp(n, BAD_CAST "dataItemId");
    value = xmlNodeGetContent(n);

    printf("Found: %s:%s with value %s\n", 
           n->name, name, value);
    xmlFree(value);
    xmlFree(name);
  }

  xmlXPathFreeObject(nodes);    
  xmlXPathFreeContext(xpathCtx);
  xmlFreeDoc(document);
}