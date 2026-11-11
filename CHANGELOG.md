# What's New in ygol-access

The newest version comes first. Each version lists the features and fixes you'll notice; small internal changes are left out.

## Unreleased

### New

- **Picks in card lists are confirmed.** When you pick cards from a list in a duel, each pick is read, with the running count once you have more than one: "Archfiend Soldier selected", "Air Eater selected, 2 selected", "Archfiend Soldier deselected". Before, nothing told you whether a card was picked.
- **Pendulum Zones.** A Pendulum card in spell and trap zone 1 or 5 is read as being in a Pendulum Zone, with its Scale: "Flash Knight, your Pendulum Zone 1, Scale 7", "You activate Performapal Turn Toad in Pendulum Zone 2".

### Fixed

- **Xyz Materials.** An Xyz Monster's materials are now read in Show Card List and when you pick which ones to detach: "Aqua Snake, Xyz Material of your monster zone 3". Before, these lists were silent. Detaching reads "Your Aqua Snake is detached from monster zone 3 and goes to Graveyard". An Xyz Summon no longer reads its materials moving as "Your card goes from monster zone 3 to monster zone 3".
- **Pendulum Summons from the hand.** The list of hand cards to Pendulum Summon now reads each card as you move: "Archfiend Soldier, your hand". The position question names the card it is for: "Select the battle position of the card. Air Eater, ATK 2100, DEF 1600".
- **Tutorial setups read as draws.** When a tutorial sets the duel up again partway through, the hand it deals is no longer read as a string of "You draw…" lines.

## 0.1.4 (November 5, 2026)

### Fixed

- **Numbers in the Tutorial list.** Scrolling past the rows on screen in Help & Options > Tutorial now reads the number with the name: "11, Display Card List". Before, only the name was read.
- **The Controls screen.** In Help & Options > Controls, the Keyboard tab now reads each function and its keys as you move: "Display Command Menu/Confirm, Left click, Enter or A". Before, only "Un-assign a key" was read. Assigning a key reads the row's new keys, and the warning when you try to remove a function's only key is read too.

## 0.1.3 (November 4, 2026)

### Fixed

- **I reading a different card in the trunk.** Moving right from the last card in a row onto the next one, in the trunk or Related Cards, could make I read the card you'd just left instead of the one you're on. This kept happening every row until you moved left or right. I now reads the card under the cursor.

## 0.1.2 (November 4, 2026)

### Fixed

- **The tutorial's exit question.** Pressing S in a tutorial duel now reads the question before the button you're on: "Would you like to exit this tutorial?", then "No". Before, only "No" was read.
- **Alt hotkeys opening the Windows menu.** Alt with F, U, G, V or E still reads the opponent's field, hand, Graveyard, banished cards or Extra Deck. Letting go of Alt no longer opens the game window's system menu.
- **The first card in a Deck or Extra Deck list.** When you view or summon from your Extra Deck (or pick from another pile), the first card in the list is now read as the list opens and when you move back to it: "Junk Warrior, your Extra Deck". Before, it was skipped.

## 0.1.1 (November 3, 2026)

### Fixed

- **Card levels read while scrolling a list.** Picking a card from your Deck, Graveyard or Extra Deck in a duel no longer reads stray levels like "x4" as you move through the list. You hear only the card names ("Elemental HERO Sparkman, your Deck").
- **Effect choices called "not available" by mistake.** In a "Use which effect?" box, the choices you can use were read as "not available", and the ones you can't use as available. It's now the right way round: for Elemental HERO Stratos with no other HERO monsters, "Choice 1 of 3, not available: Destroy Spells/Traps…", then "Choice 2 of 3: Add 1 "HERO" monster from your Deck to your hand."
- **Yes/No questions read in the wrong order.** Duel questions are read before the button you're on, without the other button tacked on: "Activate the effect of "Vision HERO Faris"?", then "Yes".
- **I at a duel question.** Pressing I at "Activate the effect of …?" reads that card instead of "No card selected".
- **The summoned card repeated during material picks.** While you pick Link or Fusion Materials, the monster being summoned is said once, not again after each material.
- **Show Details read without the card's name.** Show Details now reads the card's full info, like I: name, type, stats and text. Before, it started partway into the text ("2 Warrior monsters…").
- **Doubled labels on the reward screen.** Rewards after a duel are read once ("NEW CARD AWARD! Evo-Force", "VICTORY DUEL POINTS EARNED! 1060 DP"), without "Card Award" or "DP" said first.

## 0.1.0 (November 3, 2026)

The first release.

### New

- **Speech through your screen reader.** Everything is read through NVDA, JAWS or another screen reader, or through Windows speech when none is running. The mod says "Yu-Gi-Oh accessibility mod loaded. Press F1 for hotkeys." when the game starts.
- **Menus and settings.** The selected item is read, then its description. Setting values, volume levels, dialog boxes and on-screen key prompts ("Enter or A") are read too.
- **Campaign.** The series, the duel lists and the story dialogue are read, line by line with the speaker's name.
- **Cards in menus.** Moving onto a card reads its name, "New" if you haven't looked at it yet, and its place ("3 of 8"). I reads its full info: type, stats, text and how many copies you own.
- **Deck editor.** The sections with their card counts ("Extra, 0 cards"), the trunk, related cards, sorting ("Sort by Card Frame", then the card under the cursor), the full card view and the filter panel ("Selected", "Not selected"). R reads the deck's counts ("Deck: 23 Effect, 12 Fusion, 15 Spell, 2 Trap").
- **Shop.** Each pack's name, price and how much of it you own ("12 of 40 cards, 20 of 120 copies"), the purchase prompt, your DP after buying, and each card you pull.
- **Duel cursor.** Moving over a card reads its name, position, place and current ATK and DEF, with the printed values when an effect changed them: "Elemental HERO Knospe, attack, your monster zone 4, ATK 700 (original 600), DEF 900 (original 1000)". Empty zones and piles are read too: "Your Graveyard, 2 cards, top Garoozis".
- **What happens in a duel.** Turns, phases and life points ("Turn 3, your turn. Your Draw Phase", "You lose 1600 LP, now 6400"), and both duelists' draws, summons, Sets, activations, chains, attacks, cards moving between zones, flips, position changes, targets, coin tosses, dice rolls and shuffles. The opponent's hidden cards are never named.
- **Duel choices.** The action menu, zone and target prompts with the card under the cursor ("Select an attack target monster. Ancient Gear, attack, the opponent's monster zone 3, ATK 100, DEF 800"), phase selection, effect choices, Yes/No and OK boxes, and lists of cards to pick from.
- **Summon materials.** Each Tribute or material is read as you pick it ("Elemental HERO Avian selected as Fusion Material") and again when it is used.
- **Duel results.** The win or loss, the result screen and the rewards.
- **Hotkeys.** F1 lists them. F5 reads the whole screen, R your name and DP, H both duelists' life points, I the full info of the card you are on. In a duel: F the field, U the hand, G the Graveyard, V banished cards, E the Extra Deck and T the turn and phase; hold Alt for the opponent's.
- **Game update warning.** If a game update moves parts of the game the mod relies on, it says so at startup, so you know why some speech is missing.
- **Quitting no longer freezes the game.** The game could hang when you quit; the mod fixes this.
