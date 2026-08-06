#ifndef COS_CPP_SDK_V5_INCLUDE_RAPIDXML_FWD_H_
#define COS_CPP_SDK_V5_INCLUDE_RAPIDXML_FWD_H_

// RapidXML is bundled with the SDK, but its implementation is not needed by
// consumers of the public request/response declarations.  Keep the public
// headers lightweight and include rapidxml.hpp only in implementation files.
#ifndef RAPIDXML_HPP_INCLUDED
#define COS_CPP_SDK_RAPIDXML_FWD_HAS_DEFAULT
namespace rapidxml {
template <class Ch = char>
class xml_node;

template <class Ch = char>
class xml_document;
}  // namespace rapidxml
#endif

#endif  // COS_CPP_SDK_V5_INCLUDE_RAPIDXML_FWD_H_
