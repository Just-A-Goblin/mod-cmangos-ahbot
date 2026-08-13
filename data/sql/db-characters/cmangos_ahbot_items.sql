-- Per-item override table for mod-cmangos-ahbot.
-- Lives in the CHARACTERS database (CMaNGOS keeps ahbot_items there; plan §7).
--   value      : fixed per-unit price base. 0 => blacklist (item never listed).
--   add_chance : >0 => inject this item every pass at this % chance, bypassing
--                the natural loot roll (min_amount..max_amount count).
--   min/max    : injection count range for add_chance.
CREATE TABLE IF NOT EXISTS `cmangos_ahbot_items`
(
    `item`       INT UNSIGNED NOT NULL,
    `value`      INT UNSIGNED NOT NULL DEFAULT 0,
    `add_chance` INT UNSIGNED NOT NULL DEFAULT 0,
    `min_amount` INT UNSIGNED NOT NULL DEFAULT 0,
    `max_amount` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`item`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
