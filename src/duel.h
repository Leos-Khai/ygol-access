#pragma once

#include <string>

// Duel speech: engine events (turns, phases, LP, summons, attacks, chains, results) as they are shown, and
// board review on hotkeys. Needs MenuHooksInstall first (card names).
bool DuelHooksInstall();

// Board review, side 0 = you, 1 = the opponent. Each speaks "not in a duel" outside one.
void DuelSpeakField(int side);
void DuelSpeakHand(int side);
void DuelSpeakGraveyard(int side);
void DuelSpeakBanished(int side);
void DuelSpeakExtraDeck(int side);
void DuelSpeakTurn();

// Whether a duel is on screen (its dialogs' buttons are spoken on their own).
bool DuelInProgress();

// What is under the duel cursor ("Your monster zone 3, empty") when a duel prompt is about it (a zone picker, the
// card the prompt names, a card to activate); empty otherwise. Game thread.
std::wstring DuelCursorText(const std::wstring& prompt);

// Text about to be spoken, with duel context added: an effect choice's description gets "Choice 2 of 3: " (and
// "not available" when the choice can't be used). Any thread.
std::wstring DuelAnnotate(const std::wstring& text);
