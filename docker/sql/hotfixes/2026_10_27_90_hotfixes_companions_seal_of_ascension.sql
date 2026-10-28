-- Companions: the Seal of Ascension (12344), the key of Upper Blackrock Spire. The 1.60.1 client has the ring in Item.db2
-- but not in ItemSparse, so the server did not know it: the quest "Seal of Ascension" gave nothing and the Dragonspine Door
-- (opened by a player carrying the seal, classic_instance_blackrock_spire) could never open. Built like
-- 2026_10_26_90_hotfixes_companions_vanilla_items.sql (TDB 335 item_template and locales), with its own hotfix push
-- (2610271) so the items already sent to the game keep their ids.
DELETE FROM `item_sparse` WHERE `ID` IN (12344);
DELETE FROM `item_sparse_locale` WHERE `ID` IN (12344);
DELETE FROM `item_search_name` WHERE `ID` IN (12344);
DELETE FROM `item_search_name_locale` WHERE `ID` IN (12344);
DELETE FROM `item_appearance` WHERE `ID` BETWEEN 9930001 AND 10029999;
DELETE FROM `item_modified_appearance` WHERE `ID` BETWEEN 9930001 AND 10029999;
DELETE FROM `item_effect` WHERE `ID` BETWEEN 9930001 AND 10029999;
DELETE FROM `item_x_item_effect` WHERE `ID` BETWEEN 9930001 AND 10029999;
DELETE FROM `hotfix_data` WHERE `Id` = 2610271;
INSERT INTO `item_sparse` (`ID`,`Description`,`Display3`,`Display2`,`Display1`,`Display`,`ExpansionID`,`DmgVariance`,`LimitCategory`,`DurationInInventory`,`QualityModifier`,`BagFamily`,`StartQuestID`,`LanguageID`,`ItemRange`,`StatPercentageOfSocket1`,`StatPercentageOfSocket2`,`StatPercentageOfSocket3`,`StatPercentageOfSocket4`,`StatPercentageOfSocket5`,`StatPercentageOfSocket6`,`StatPercentageOfSocket7`,`StatPercentageOfSocket8`,`StatPercentageOfSocket9`,`StatPercentageOfSocket10`,`StatPercentEditor1`,`StatPercentEditor2`,`StatPercentEditor3`,`StatPercentEditor4`,`StatPercentEditor5`,`StatPercentEditor6`,`StatPercentEditor7`,`StatPercentEditor8`,`StatPercentEditor9`,`StatPercentEditor10`,`StatModifierBonusStat1`,`StatModifierBonusStat2`,`StatModifierBonusStat3`,`StatModifierBonusStat4`,`StatModifierBonusStat5`,`StatModifierBonusStat6`,`StatModifierBonusStat7`,`StatModifierBonusStat8`,`StatModifierBonusStat9`,`StatModifierBonusStat10`,`Stackable`,`MaxCount`,`MinReputation`,`RequiredAbility`,`AllowableRace1`,`AllowableRace2`,`SellPrice`,`BuyPrice`,`VendorStackCount`,`PriceVariance`,`PriceRandomValue`,`Flags1`,`Flags2`,`Flags3`,`Flags4`,`Flags5`,`FactionRelated`,`ModifiedCraftingReagentItemID`,`ContentTuningID`,`PlayerLevelToItemLevelCurveID`,`ItemLevelOffsetCurveID`,`ItemLevelOffsetItemLevel`,`ItemSquishEraID`,`ItemNameDescriptionID`,`RequiredTransmogHoliday`,`RequiredHoliday`,`GemProperties`,`SocketMatchEnchantmentId`,`TotemCategoryID`,`InstanceBound`,`ZoneBound1`,`ZoneBound2`,`ItemSet`,`LockID`,`PageID`,`ItemDelay`,`MinFactionID`,`RequiredSkillRank`,`RequiredSkill`,`ItemLevel`,`AllowableClass`,`ArtifactID`,`SpellWeight`,`SpellWeightCategory`,`SocketType1`,`SocketType2`,`SocketType3`,`SheatheType`,`Material`,`PageMaterialID`,`Bonding`,`DamageDamageType`,`ContainerSlots`,`RequiredPVPMedal`,`RequiredPVPRank`,`RequiredLevel`,`InventoryType`,`OverallQualityID`,`AmmunitionType`,`VerifiedBuild`) VALUES
(12344,'Draconic runes appear and disappear along the inner band.','','','','Seal of Ascension',0,0.0,0,0,0.0,0,0,0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,6000,4500,4500,0,0,0,0,0,0,0,7,31,32,-1,-1,-1,-1,-1,-1,-1,1,1,0,0,-1,-1,0,0,1,1.0,1.0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,61,-1,0,0,0,0,0,0,0,3,0,1,0,0,0,0,0,11,3,0,0);
INSERT INTO `item_sparse_locale` (`ID`,`locale`,`Description_lang`,`Display3_lang`,`Display2_lang`,`Display1_lang`,`Display_lang`,`VerifiedBuild`) VALUES
(12344,'deDE','Entlang des inneren Bandes erscheinen drakonische Runen und verschwinden dann wieder.','','','','Siegel des Aufstiegs',0),
(12344,'esES','Aparecen y desaparecen runas dracónicas en la banda interior.','','','','Sello de ascensión',0),
(12344,'esMX','Aparecen y desaparecen runas dracónicas en la banda interior.','','','','Sello de ascensión',0),
(12344,'frFR','Des runes draconiques semblent aller et venir à l''intérieur de l''anneau.','','','','Sceau d''ascension',0),
(12344,'koKR','안쪽 테두리를 따라 용족 룬문자가 나타났다 사라졌다 합니다.','','','','승천의 인장',0),
(12344,'ruRU','Руны драконов на внутренней стороне, то пропадают, то снова появляются.','','','','Печать вознесения',0),
(12344,'zhCN','龙类的符文在它的表面上不断浮现。','','','','晋升印章',0),
(12344,'zhTW','龍類的符文在它的表面上不斷浮現。','','','','晉升徽印',0);
INSERT INTO `item_search_name` (`ID`,`Display`,`OverallQualityID`,`ExpansionID`,`MinFactionID`,`MinReputation`,`AllowableClass`,`RequiredLevel`,`RequiredSkill`,`RequiredSkillRank`,`RequiredAbility`,`ItemLevel`,`Flags1`,`Flags2`,`Flags3`,`Flags4`,`Flags5`,`AllowableRace1`,`AllowableRace2`,`VerifiedBuild`) VALUES
(12344,'Seal of Ascension',3,0,0,0,-1,0,0,0,0,61,0,4,0,0,0,-1,-1,0);
INSERT INTO `item_search_name_locale` (`ID`,`locale`,`Display_lang`,`VerifiedBuild`) VALUES
(12344,'deDE','Siegel des Aufstiegs',0),
(12344,'esES','Sello de ascensión',0),
(12344,'esMX','Sello de ascensión',0),
(12344,'frFR','Sceau d''ascension',0),
(12344,'koKR','승천의 인장',0),
(12344,'ruRU','Печать вознесения',0),
(12344,'zhCN','晋升印章',0),
(12344,'zhTW','晉升徽印',0);
INSERT INTO `hotfix_data` (`Id`,`UniqueId`,`TableHash`,`RecordId`,`Status`,`VerifiedBuild`) VALUES
(2610271,2610271,2442913102,12344,1,0),
(2610271,2610271,428746933,12344,1,0);
