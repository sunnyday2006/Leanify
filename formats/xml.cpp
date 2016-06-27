#include "xml.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "../leanify.h"
#include "../utils.h"
#include "base64.h"

using std::cout;
using std::endl;
using std::string;

Xml::Xml(void* p, size_t s /*= 0*/) : Format(p, s) {
  pugi::xml_parse_result result = doc_.load_buffer(
      fp_, size_, pugi::parse_default | pugi::parse_declaration | pugi::parse_doctype | pugi::parse_ws_pcdata_single);
  is_valid_ = result;
  encoding_ = result.encoding;
}

namespace {
// https://github.com/svg/svgo/blob/master/plugins/_collections.js
const std::map<string, string> kDefaultAttributes = { { "x", "0" },
                                                      { "y", "0" },
                                                      { "width", "100%" },
                                                      { "height", "100%" },
                                                      { "clip", "auto" },
                                                      { "clip-path", "none" },
                                                      { "clip-rule", "nonzero" },
                                                      { "mask", "none" },
                                                      { "opacity", "1" },
                                                      { "solid-color", "#000" },
                                                      { "solid-opacity", "1" },
                                                      { "stop-color", "#000" },
                                                      { "stop-opacity", "1" },
                                                      { "fill-opacity", "1" },
                                                      { "fill-rule", "nonzero" },
                                                      { "fill", "#000" },
                                                      { "stroke", "none" },
                                                      { "stroke-width", "1" },
                                                      { "stroke-linecap", "butt" },
                                                      { "stroke-linejoin", "miter" },
                                                      { "stroke-miterlimit", "4" },
                                                      { "stroke-dasharray", "none" },
                                                      { "stroke-dashoffset", "0" },
                                                      { "stroke-opacity", "1" },
                                                      { "paint-order", "normal" },
                                                      { "vector-effect", "none" },
                                                      { "viewport-fill", "none" },
                                                      { "viewport-fill-opacity", "1" },
                                                      { "display", "inline" },
                                                      { "visibility", "visible" },
                                                      { "marker-start", "none" },
                                                      { "marker-mid", "none" },
                                                      { "marker-end", "none" },
                                                      { "color-interpolation", "sRGB" },
                                                      { "color-interpolation-filters", "linearRGB" },
                                                      { "color-rendering", "auto" },
                                                      { "shape-rendering", "auto" },
                                                      { "text-rendering", "auto" },
                                                      { "image-rendering", "auto" },
                                                      { "buffered-rendering", "auto" },
                                                      { "font-style", "normal" },
                                                      { "font-variant", "normal" },
                                                      { "font-weight", "normal" },
                                                      { "font-stretch", "normal" },
                                                      { "font-size", "medium" },
                                                      { "font-size-adjust", "none" },
                                                      { "kerning", "auto" },
                                                      { "letter-spacing", "normal" },
                                                      { "word-spacing", "normal" },
                                                      { "text-decoration", "none" },
                                                      { "text-anchor", "start" },
                                                      { "text-overflow", "clip" },
                                                      { "writing-mode", "lr-tb" },
                                                      { "glyph-orientation-vertical", "auto" },
                                                      { "glyph-orientation-horizontal", "0deg" },
                                                      { "direction", "ltr" },
                                                      { "unicode-bidi", "normal" },
                                                      { "dominant-baseline", "auto" },
                                                      { "alignment-baseline", "baseline" },
                                                      { "baseline-shift", "baseline" },
                                                      { "slope", "1" },
                                                      { "intercept", "0" },
                                                      { "amplitude", "1" },
                                                      { "exponent", "1" },
                                                      { "offset", "0" } };

// shrink spaces and newlines, also removes preceding and trailing spaces
string ShrinkSpace(const char* value) {
  string new_value;
  while (*value) {
    if (*value == ' ' || *value == '\n' || *value == '\t') {
      do {
        value++;
      } while (*value == ' ' || *value == '\n' || *value == '\t');

      if (*value == 0)
        break;
      new_value += ' ';
    }
    new_value += *value++;
  }
  if (!new_value.empty() && new_value[0] == ' ')
    new_value.erase(0, 1);

  return new_value;
}

void TraverseElements(pugi::xml_node node, std::function<void(pugi::xml_node)> callback) {
  // cannot use ranged loop here because we need to store the next_sibling before recursion so that if child was removed
  // the loop won't be terminated
  for (pugi::xml_node child = node.first_child(), next; child; child = next) {
    next = child.next_sibling();
    TraverseElements(child, callback);
  }

  callback(node);
}

// Remove single PCData only contains whitespace if xml:space="preserve" is not set.
void RemovePCDataSingle(pugi::xml_node node, bool xml_space_preserve) {
  xml_space_preserve |= strcmp(node.attribute("xml:space").value(), "preserve") == 0;
  if (xml_space_preserve)
    return;

  pugi::xml_node pcdata = node.first_child();
  if (pcdata.first_child() == nullptr && pcdata.type() == pugi::node_pcdata && pcdata.next_sibling() == nullptr) {
    if (ShrinkSpace(pcdata.value()).empty())
      node.remove_child(pcdata);
    return;
  }

  for (pugi::xml_node child : node.children())
    RemovePCDataSingle(child, xml_space_preserve);
}

struct xml_memory_writer : pugi::xml_writer {
  uint8_t* p_write;

  void write(const void* data, size_t size) override {
    memcpy(p_write, data, size);
    p_write += size;
  }
};
}  // namespace

size_t Xml::Leanify(size_t size_leanified /*= 0*/) {
  RemovePCDataSingle(doc_, false);

  // if the XML is fb2 file
  if (doc_.child("FictionBook")) {
    if (is_verbose) {
      cout << "FB2 detected." << endl;
    }
    if (depth < max_depth) {
      depth++;

      pugi::xml_node root = doc_.child("FictionBook");

      // iterate through all binary element
      for (pugi::xml_node binary = root.child("binary"), next; binary; binary = next) {
        next = binary.next_sibling("binary");
        pugi::xml_attribute id = binary.attribute("id");
        if (id == nullptr || id.value() == nullptr || id.value()[0] == 0) {
          root.remove_child(binary);
          continue;
        }

        PrintFileName(id.value());

        const char* base64_data = binary.child_value();
        if (base64_data == nullptr || base64_data[0] == 0) {
          cout << "No data found." << endl;
          continue;
        }
        size_t base64_len = strlen(base64_data);
        // copy to a new location because base64_data is const
        std::vector<char> new_base64_data(base64_data, base64_data + base64_len + 1);

        size_t new_base64_len = Base64(new_base64_data.data(), base64_len).Leanify();

        if (new_base64_len < base64_len) {
          new_base64_data[new_base64_len] = 0;
          binary.text() = new_base64_data.data();
        }
      }
      depth--;
    }
  } else if (doc_.child("svg")) {
    if (is_verbose) {
      cout << "SVG detected." << endl;
    }

    // remove XML declaration and doctype
    for (pugi::xml_node child = doc_.first_child(), next; child; child = next) {
      next = child.next_sibling();

      if (child.type() == pugi::node_declaration || child.type() == pugi::node_doctype)
        doc_.remove_child(child);
    }

    TraverseElements(doc_.child("svg"), [](pugi::xml_node node) {
      for (pugi::xml_attribute attr = node.first_attribute(), next; attr; attr = next) {
        next = attr.next_attribute();

        string value = ShrinkSpace(attr.value());
        // Remove empty attribute
        if (value.empty()) {
          node.remove_attribute(attr);
          continue;
        }

        // Remove default attribute
        auto it = kDefaultAttributes.find(attr.name());
        if (it != kDefaultAttributes.end() && it->second == value) {
          // Only remove it if it's not an override of a parent non-default attribute
          bool is_override = false;
          for (pugi::xml_node n = node.parent(); n; n = n.parent()) {
            pugi::xml_attribute n_attr = n.attribute(attr.name());
            // Stop at the first parent that has the same attribute, we can safely remove the attribute in the current
            // node if this parent has the same value,
            // even if there's another parent higher up in the tree which has a different value for this attribute
            // (in which case we won't be able to remove the default attribute in this parent).
            if (!n_attr.empty()) {
              if (n_attr.value() != value)
                is_override = true;
              break;
            }
          }
          if (!is_override) {
            node.remove_attribute(attr);
            continue;
          }
        }

        attr = value.c_str();
      }

      // remove empty text element and container element
      const char* name = node.name();
      if (node.first_child() == nullptr) {
        if (strcmp(name, "text") == 0 || strcmp(name, "tspan") == 0 || strcmp(name, "a") == 0 ||
            strcmp(name, "defs") == 0 || strcmp(name, "g") == 0 || strcmp(name, "marker") == 0 ||
            strcmp(name, "mask") == 0 || strcmp(name, "missing-glyph") == 0 || strcmp(name, "pattern") == 0 ||
            strcmp(name, "switch") == 0 || strcmp(name, "symbol") == 0) {
          node.parent().remove_child(node);
          return;
        }
      }

      if (strcmp(name, "tref") == 0 && node.attribute("xlink:href").empty()) {
        node.parent().remove_child(node);
        return;
      }

      if (strcmp(name, "metadata") == 0) {
        node.parent().remove_child(node);
        return;
      }
    });
  }

  // print leanified XML to memory
  xml_memory_writer writer;
  fp_ -= size_leanified;
  writer.p_write = fp_;
  doc_.save(writer, nullptr, pugi::format_raw | pugi::format_no_declaration, encoding_);
  size_ = writer.p_write - fp_;
  return size_;
}
