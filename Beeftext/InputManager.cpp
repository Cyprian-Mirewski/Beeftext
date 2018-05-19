/// \file
/// \author Xavier Michelon
///
/// \brief Declaration of input manager
///  
/// Copyright (c) Xavier Michelon. All rights reserved.  
/// Licensed under the MIT License. See LICENSE file in the project root for full license information.  


#include "stdafx.h"
#include "InputManager.h"
#include "PreferencesManager.h"
#include "MainWindow.h"
#include <XMiLib/Exception.h>


using namespace xmilib;


namespace {
InputManager::KeyStroke const kNullKeyStroke = {0, 0, {0}}; ///< A null keystroke
qint32 const kTextBufferSize = 10;
///< The size of the buffer that will receive the text resulting from the processing of the key stroke
}


bool getForegroundWindowInputLocale(HKL& outHkl); ///< Retrieve the Input local of the currently focused window
bool isComboTriggerShortcut(InputManager::KeyStroke const& keyStroke); ///< Check if a keystroke is the manual substitution shortcut


//**********************************************************************************************************************
/// \param[out] outHkl If the function returns true, this variable holds the input local of the foreground window. If
/// the function returns false, the value of this variable is undetermined on function exit.
/// \return true if and only if the input local of the foreground window could be determined
//**********************************************************************************************************************
bool getForegroundWindowInputLocale(HKL& outHkl)
{
   // ReSharper disable once CppLocalVariableMayBeConst
   HWND hwnd = GetForegroundWindow();
   if (!hwnd)
      return false;
   outHkl = GetKeyboardLayout(GetWindowThreadProcessId(hwnd, nullptr));
   return true;
}


//**********************************************************************************************************************
/// \return true if and only if keystroke correspond to the shortcut
//**********************************************************************************************************************
bool isComboTriggerShortcut(InputManager::KeyStroke const& keyStroke)
{
   SpShortcut const shortcut = PreferencesManager::instance().comboTriggerShortcut();
   if (!shortcut)
      return false;
   if (keyStroke.virtualKey != shortcut->nativeVirtualKey())
      return false;
   Qt::KeyboardModifiers modifiers = shortcut->nativeModifiers();
   quint8 const* ks = keyStroke.keyboardState;
   return bool((ks[VK_LCONTROL] & 0x80) || (ks[VK_RCONTROL] & 0x80)) == modifiers.testFlag(Qt::ControlModifier)
      && bool((ks[VK_LMENU] & 0x80) || (ks[VK_RMENU] & 0x80)) == modifiers.testFlag(Qt::AltModifier)
      && bool((ks[VK_LWIN] & 0x80) || (ks[VK_RWIN] & 0x80)) == modifiers.testFlag(Qt::MetaModifier)
      && bool((ks[VK_LSHIFT] & 0x80) || (ks[VK_RSHIFT] & 0x80)) == modifiers.testFlag(Qt::ShiftModifier);
}


//**********************************************************************************************************************
/// This static member function is registered to be called whenever a key event occurs.
/// 
/// \param[in] nCode A code the hook procedure uses to determine how to process the message
/// \param[in] wParam The identifier of the keyboard message
/// \param[in] lParam A pointer to a KBDLLHOOKSTRUCT structure.
//**********************************************************************************************************************
LRESULT CALLBACK InputManager::keyboardProcedure(int nCode, WPARAM wParam, LPARAM lParam)
{
   if ((WM_KEYDOWN == wParam) || (WM_SYSKEYDOWN == wParam))
   {
      KeyStroke keyStroke = kNullKeyStroke;
      KBDLLHOOKSTRUCT* keyEvent = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

      // we ignore shift / caps lock key events
      if ((keyEvent->vkCode == VK_LSHIFT) || (keyEvent->vkCode == VK_RSHIFT) || (keyEvent->vkCode == VK_CAPITAL))
         return CallNextHookEx(nullptr, nCode, wParam, lParam);
      keyStroke.virtualKey = keyEvent->vkCode;
      keyStroke.scanCode = keyEvent->scanCode;
      // GetKeyboardState() do not properly report state for modifier keys if the key event in a window other that one
      // from the current process, so we need to manually fetch the valid states manually using GetKeyState()
      // We do not actually need the state of the other key, so we do not event bother calling GetKeyboardState()
      QList<quint32> keyList = {
         VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL,
         VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU, VK_RWIN, VK_LWIN, VK_CAPITAL
      };
      for (qint32 key: keyList)
         keyStroke.keyboardState[key] = GetKeyState(key);

      // our event handler will return false if we want to 'intercept' the keystroke and not pass it to the next hook,
      // but the MSDN documentation says we MUST do it if nCode < 0
      if ((!instance().onKeyboardEvent(keyStroke)) && (nCode >= 0))
         return 0;
   }
   return CallNextHookEx(nullptr, nCode, wParam, lParam);
}


//**********************************************************************************************************************
/// This static member function is registered to be called whenever a mouse event occurs.
/// 
/// \param[in] nCode A code the hook procedure uses to determine how to process the message
/// \param[in] wParam The identifier of the mouse message
/// \param[in] lParam A pointer to a M structure.
//**********************************************************************************************************************
LRESULT CALLBACK InputManager::mouseProcedure(int nCode, WPARAM wParam, LPARAM lParam)
{
   if ((WM_LBUTTONDOWN == wParam) || (WM_RBUTTONDOWN == wParam) || (WM_MOUSEWHEEL == wParam) ||
      (WM_MBUTTONDOWN == wParam)) // note we consider mouse wheel moves as clicks
   {
      instance().onMouseClickEvent(nCode, wParam, lParam);
   }
   return CallNextHookEx(nullptr, nCode, wParam, lParam);
}


//**********************************************************************************************************************
/// \return The only allowed instance of the class
//**********************************************************************************************************************
InputManager& InputManager::instance()
{
   static InputManager instance;
   return instance;
}


//**********************************************************************************************************************
///
//**********************************************************************************************************************
InputManager::InputManager()
   : QObject(nullptr),
     keyboardHook_(nullptr),
     mouseHook_(nullptr),
     deadKey_(kNullKeyStroke)
{
   this->enableKeyboardHook();
#ifdef NDEBUG
   // to avoid being locked with all input unresponsive when in debug (because one forgot that breakpoints should be
   // avoided, for instance), we only enable the low level mouse hook in release configuration
   this->enableMouseHook();
#endif
}


//**********************************************************************************************************************
// 
//**********************************************************************************************************************
InputManager::~InputManager()
{
   this->disableKeyboardHook();
   this->disableMouseHook();
}


//**********************************************************************************************************************
/// \param[in] keyStroke The key stroke
/// \return true if the event can be passed down to the keyboard hooked chain, and false it it should be removed
//**********************************************************************************************************************
bool InputManager::onKeyboardEvent(KeyStroke const& keyStroke)
{
   if ((!PreferencesManager::instance().useAutomaticSubstitution()) && isComboTriggerShortcut(keyStroke))
   {
      emit substitutionShortcutTriggered();
      return false;
   }

   // on some layout (e.g. US International, direction key + alt lead to garbage char if ToUnicode is pressed, so
   // we bypass normal processing for those keys(note this is different for the dead key issue described in
   // processKey().

   if ((VK_UP == keyStroke.virtualKey) || (VK_RIGHT == keyStroke.virtualKey) || (VK_DOWN == keyStroke.virtualKey) ||
      (VK_LEFT == keyStroke.virtualKey))
   {
      emit comboBreakerTyped();
      return true;
   }

   bool isDeadKey = false;
   QString text = this->processKey(keyStroke, isDeadKey);
   if (text.isEmpty())
      return true;

   for (QChar c: text)
   {
      if (QChar('\b') == c)
      {
         emit backspaceTyped();
         continue;
      }
      if (c.isSpace() || (!c.isPrint()))
      emit comboBreakerTyped();
      else
      emit characterTyped(c);
   }
   return true;
}


//**********************************************************************************************************************
/// This function is more complex than it should be because dead keys that are present on layouts such as 
/// the US-International keyboard require a special treatment.
///
/// \param[in] keyStroke The key stroke
/// \param[out] outIsDeadKey Is the key a dead key
/// \return The text resulting of the keystroke
//**********************************************************************************************************************
QString InputManager::processKey(KeyStroke const& keyStroke, bool& outIsDeadKey)
{
   // The core of this function is the call to ToUnicodeEx() - or ToUnicode() - who transforms a keystroke into 
   // an actual text output, taking into account the current input locale (a.k.a. keyboard layout).
   // now the tricky part: ToUnicode() "consumes" the dead key that may be stored in the kernel-mode keyboard buffer
   // so we need to manually restore the dead key by calling ToUnicode() again
   WCHAR textBuffer[kTextBufferSize];
   outIsDeadKey = false;
   HKL hkl = nullptr;

   // Windows allow each window to have its own input locale, so we try to obtain the locale (HKL) of the active window
   // andpass it to ToUnicodeEx(). If we fail to do so we call ToUnicode instead, which use the system-wide locale
   qint32 const size = getForegroundWindowInputLocale(hkl)
      ? ToUnicodeEx(keyStroke.virtualKey, keyStroke.scanCode, keyStroke.keyboardState, textBuffer, kTextBufferSize, 0
         , hkl) : ToUnicode(keyStroke.virtualKey, keyStroke.scanCode, keyStroke.keyboardState, textBuffer
         , kTextBufferSize, 0);

   if (-1 == size)
   {
      // the key is a dead key. We have consumed it so we need to:
      // 1 - Restore it by repeating the call to ToUnicode()
      // 2 - Save the key because we will need to apply it again before the next 'normal' keystroke
      ToUnicode(keyStroke.virtualKey, keyStroke.scanCode, keyStroke.keyboardState, textBuffer, kTextBufferSize, 0);
      deadKey_ = keyStroke;
      outIsDeadKey = true;
      return QString();
   }

   if (size > 0)
   {
      // The key is a normal key that will result in text output.
      // if the previous key was a dead key, we have already consumed the dead key so we must restore it 
      QString result = QString::fromWCharArray(textBuffer, size);
      if (0 != deadKey_.virtualKey)
      {
         ToUnicode(deadKey_.virtualKey, deadKey_.scanCode, deadKey_.keyboardState, textBuffer, kTextBufferSize, 0);
         deadKey_.virtualKey = 0;
      }
      return result;
   }

   // final case: size is 0, the key is a modifier, we do nothing
   // values of size < -1 also lead here but should not happen according to the documentation for ToUnicode()
   return QString();
}


//**********************************************************************************************************************
// 
//**********************************************************************************************************************
void InputManager::onMouseClickEvent(int, WPARAM, LPARAM)
{
   emit comboBreakerTyped();
}


//**********************************************************************************************************************
/// \return true if and only if the keyboard hook is enabled
//**********************************************************************************************************************
bool InputManager::isKeyboardHookEnable() const
{
   return keyboardHook_;
}


//**********************************************************************************************************************
/// \return true if and only if the hook was enabled
//**********************************************************************************************************************
void InputManager::enableKeyboardHook()
{
   if (keyboardHook_)
      return;
   HMODULE const moduleHandle = GetModuleHandle(nullptr);
   keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, keyboardProcedure, moduleHandle, 0);
   if (!keyboardHook_)
      throw Exception("Could not register a keyboard hook.");
}


//**********************************************************************************************************************
// 
//**********************************************************************************************************************
void InputManager::disableKeyboardHook()
{
   if (keyboardHook_)
   {
      UnhookWindowsHookEx(keyboardHook_);
      keyboardHook_ = nullptr;
   }
}


//**********************************************************************************************************************
/// \param[in] enabled The new state of the keyboard hook
/// \return true if and only if the keyboard hook was enabled before the call
//**********************************************************************************************************************
bool InputManager::setKeyboardHookEnabled(bool enabled)
{
   bool const result = keyboardHook_;
   if (enabled)
      this->enableKeyboardHook();
   else
      this->disableKeyboardHook();
   return result;
}


//**********************************************************************************************************************
/// \return true if and only if the mouse hook is enabled
//**********************************************************************************************************************
bool InputManager::isMouseHookEnabled() const
{
   return mouseHook_;
}


//**********************************************************************************************************************
// 
//**********************************************************************************************************************
void InputManager::enableMouseHook()
{
   if (mouseHook_)
      return;
   HMODULE const moduleHandle = GetModuleHandle(nullptr);
   mouseHook_ = SetWindowsHookEx(WH_MOUSE_LL, mouseProcedure, moduleHandle, 0);
   if (!mouseHook_)
      throw Exception("Could not register a mouse hook.");
}


//**********************************************************************************************************************
// 
//**********************************************************************************************************************
void InputManager::disableMouseHook()
{
   if (mouseHook_)
   {
      UnhookWindowsHookEx(mouseHook_);
      mouseHook_ = nullptr;
   }
}


//**********************************************************************************************************************
/// \param[in] enabled The new state of the mouse hook
/// \return true if and only if the mouse hook was enabled before the call 
//**********************************************************************************************************************
bool InputManager::setMouseHookEnabled(bool enabled)
{
   bool const result = mouseHook_;
   if (enabled)
      this->enableMouseHook();
   else
      this->disableMouseHook();
   return result;
}
