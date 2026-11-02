#pragma once

// MSVC RTTI lookups in YuGiOh.exe. Class names are decorated: ".?AVwidget_Base@RIX@@" is RIX::widget_Base.

// Returns the vtable of the class with this decorated name (primary base, offset 0), or nullptr.
void** FindVTable(const char* decoratedName);

// Returns the decorated class name of a polymorphic object, or nullptr if it doesn't look like one.
const char* ClassName(const void* object);
