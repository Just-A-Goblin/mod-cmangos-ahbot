-- Additive craft-layer columns for the per-item override table (crafting addendum §8.2).
-- Idempotent (ADD COLUMN IF NOT EXISTS, MariaDB) so the module's auto-apply is safe to
-- re-run. Existing value/add_chance semantics are unchanged and apply to craft products.
--   craft_weight : production demand weight multiplier %. NULL = no override (neutral 100);
--                  0 = never craft this item; else scales its §5.2 production weight.
--   craft_margin : production margin %. NULL = use the rarity/cooldown default.
ALTER TABLE `cmangos_ahbot_items`
    ADD COLUMN IF NOT EXISTS `craft_weight` INT UNSIGNED NULL,
    ADD COLUMN IF NOT EXISTS `craft_margin` INT UNSIGNED NULL;
