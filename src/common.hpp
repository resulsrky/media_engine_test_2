#pragma once
#include <gst/gst.h>
#include <string>
#include <iostream>

#define CHECK_ELEM(x, name) \
if (!(x)) { std::cerr << "Element create failed: " << (name) << std::endl; return nullptr; }

inline void set_str(GstElement* e, const char* prop, const std::string& v) {
  g_object_set(G_OBJECT(e), prop, v.c_str(), NULL);
}
inline void set_int(GstElement* e, const char* prop, int v) {
  g_object_set(G_OBJECT(e), prop, v, NULL);
}
inline void set_bool(GstElement* e, const char* prop, gboolean v) {
  g_object_set(G_OBJECT(e), prop, v, NULL);
}
inline void set_arg(GstElement* e, const char* prop, const char* val) {
  gst_util_set_object_arg(G_OBJECT(e), prop, val);
}
