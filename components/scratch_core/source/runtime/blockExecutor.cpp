#include "blockExecutor.hpp"
#include "math.hpp"
#include "sprite.hpp"
#include "unzip.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <input.hpp>
#include <iterator>
#include <os.hpp>
#include <ratio>
#include <render.hpp>
#include <runtime.hpp>
#include <speech_manager.hpp>
#include <string>
#include <utility>
#include <vector>

#ifdef ENABLE_CLOUDVARS
#include <mist/mist.hpp>

extern std::unique_ptr<MistConnection> cloudConnection;
#endif

Timer BlockExecutor::timer;
int BlockExecutor::dragPositionOffsetX;
int BlockExecutor::dragPositionOffsetY;

// Scratch 3.0 hat re-trigger semantics (verified empirically against scratch.mit.edu):
//   event_whenkeypressed                : ignore re-trigger while running
//   event_whenbroadcastreceived         : restart (kill running thread, run from top)
//   event_whenbackdropswitchesto        : restart
//   event_whenthisspriteclicked / stage : restart
//   event_whenflagclicked               : effectively restart (stopClicked clears state first)
// isScriptRunning is for the "ignore" path (whenkeypressed only).
// restartScript is for the "restart" path — clear the chain's pending repeat
// stack so runBlock walks the hat from the top with a clean state.
static bool isScriptRunning(Sprite *sprite, Block &block) {
    auto chainIt = sprite->blockChains.find(block.blockChainID);
    return chainIt != sprite->blockChains.end() && !chainIt->second.blocksToRepeat.empty();
}

// Procedures execute on their own blockChainID with their own blocksToRepeat,
// independent of the caller. So clearing only the hat's chain leaves any
// procedure's repeat_until / forever as an orphan that runRepeatBlocks keeps
// invoking each frame. Walk the call graph and clear all transitively-called
// procedure chains before clearing the caller's own chain.
static void restartScript(Sprite *sprite, Block &block) {
    std::vector<std::string> worklist = {block.blockChainID};
    std::vector<std::string> toClear;
    while (!worklist.empty()) {
        std::string chainId = worklist.back();
        worklist.pop_back();
        if (std::find(toClear.begin(), toClear.end(), chainId) != toClear.end()) continue;
        toClear.push_back(chainId);

        auto it = sprite->blockChains.find(chainId);
        if (it == sprite->blockChains.end()) continue;
        for (Block *b : it->second.blocksToRepeat) {
            if (!b) continue;
            if (b->opcode == "procedures_call" && b->customBlockPtr != nullptr) {
                worklist.push_back(b->customBlockPtr->blockChainID);
            }
        }
    }
    for (const auto &chainId : toClear) {
        auto it = sprite->blockChains.find(chainId);
        if (it != sprite->blockChains.end()) it->second.blocksToRepeat.clear();
    }
}

std::unordered_map<std::string, BlockHandlerPtr> &BlockExecutor::getHandlers() {
    static std::unordered_map<std::string, BlockHandlerPtr> handlers;
    return handlers;
}

std::unordered_map<std::string, ValueHandlerPtr> &BlockExecutor::getValueHandlers() {
    static std::unordered_map<std::string, ValueHandlerPtr> valueHandlers;
    return valueHandlers;
}

#ifdef ENABLE_CACHING
void BlockExecutor::linkPointers(Sprite *sprite) {
    auto &h = getHandlers();
    auto &vh = getValueHandlers();

    for (auto &block : sprite->blocks) {
        // Pre-resolve block.next string ID to a direct pointer so the hot
        // dispatch loop in runBlock can avoid the per-step blocksMap lookup.
        if (!block.next.empty()) {
            auto nit = sprite->blocksMap.find(block.next);
            block.nextBlock = (nit != sprite->blocksMap.end()) ? nit->second : nullptr;
        } else {
            block.nextBlock = nullptr;
        }

        auto it = h.find(block.opcode);
        if (it != h.end()) {
            block.handler = it->second;
        } else {
            auto vit = vh.find(block.opcode);
            if (vit != vh.end()) block.valueHandler = vit->second;
        }

        for (auto &[id, input] : *block.parsedInputs) {
            if (input.inputType != ParsedInput::VARIABLE) continue;

            auto it = sprite->variables.find(input.ref);
            if (it != sprite->variables.end()) {
                input.variable = &it->second;
                continue;
            }

            auto globalIt = Scratch::stageSprite->variables.find(input.ref);
            if (globalIt != Scratch::stageSprite->variables.end()) {
                input.variable = &globalIt->second;
                continue;
            }

            input.variable = nullptr;
        }

        auto variableId = Scratch::getFieldId(block, "VARIABLE");
        if (variableId != "") {
            auto it = sprite->variables.find(variableId);
            if (it != sprite->variables.end()) {
                block.variable = &it->second;
                continue;
            }

            auto globalIt = Scratch::stageSprite->variables.find(variableId);
            if (globalIt != Scratch::stageSprite->variables.end()) {
                block.variable = &globalIt->second;
                continue;
            }

            block.variable = nullptr;
        }

        auto listId = Scratch::getFieldId(block, "LIST");
        if (listId != "") {
            auto it = sprite->lists.find(listId);
            if (it != sprite->lists.end()) {
                block.list = &it->second;
                continue;
            }

            auto globalIt = Scratch::stageSprite->lists.find(listId);
            if (globalIt != Scratch::stageSprite->lists.end()) {
                block.list = &globalIt->second;
                continue;
            }

            block.list = nullptr;
        }
    }

    for (auto &[id, monitor] : Render::monitors) {
        if (monitor.opcode == "data_variable") {
            auto it = sprite->variables.find(monitor.id);
            if (it != sprite->variables.end()) {
                monitor.variablePtr = &it->second;
                continue;
            }

            auto globalIt = Scratch::stageSprite->variables.find(monitor.id);
            if (globalIt != Scratch::stageSprite->variables.end()) {
                monitor.variablePtr = &globalIt->second;
                continue;
            }

            monitor.variablePtr = nullptr;
            continue;
        }
        if (monitor.opcode == "data_listcontents") {
            auto it = sprite->lists.find(monitor.id);
            if (it != sprite->lists.end()) {
                monitor.listPtr = &it->second;
                continue;
            }

            auto globalIt = Scratch::stageSprite->lists.find(monitor.id);
            if (globalIt != Scratch::stageSprite->lists.end()) {
                monitor.listPtr = &globalIt->second;
                continue;
            }

            monitor.variablePtr = nullptr;
            continue;
        }
    }
}
#endif

void BlockExecutor::runBlock(Block &block, Sprite *sprite, bool *withoutScreenRefresh, bool fromRepeat) {
    Block *currentBlock = &block;

    if (!sprite || sprite->toDelete) return;

    while (currentBlock && currentBlock->id != "null") {
        BlockResult result = executeBlock(*currentBlock, sprite, withoutScreenRefresh, fromRepeat);

        if (result == BlockResult::RETURN) return;

        if (currentBlock->next.empty()) return;
#ifdef ENABLE_CACHING
        // nextBlock is pre-resolved by linkPointers; falls back to map lookup
        // only if linkPointers hasn't run yet for this block (defensive).
        if (currentBlock->nextBlock) {
            currentBlock = currentBlock->nextBlock;
        } else {
            auto it = sprite->blocksMap.find(currentBlock->next);
            if (it == sprite->blocksMap.end() || !it->second) return;
            currentBlock = it->second;
        }
#else
        auto it = sprite->blocksMap.find(currentBlock->next);
        if (it == sprite->blocksMap.end() || !it->second) return;
        currentBlock = it->second;
#endif
        fromRepeat = false;
    }
}

BlockResult BlockExecutor::executeBlock(Block &block, Sprite *sprite, bool *withoutScreenRefresh, bool fromRepeat) {
#ifdef ENABLE_CACHING
    if (block.handler != nullptr) return block.handler(block, sprite, withoutScreenRefresh, fromRepeat);
#else
    const auto &h = getHandlers();
    const auto &it = h.find(block.opcode);
    if (it != h.end()) return it->second(block, sprite, withoutScreenRefresh, fromRepeat);
#endif

    if (!block.opcode.empty())
        Log::logWarning("Unknown block: " + block.opcode);

    return BlockResult::CONTINUE;
}

void BlockExecutor::executeKeyHats() {
    // Update durations: increment held keys, reset released ones.
    // O(n*m) where n=tracked keys, m=currently-held; both are tiny (<10).
    for (auto &kv : Input::keyHeldDuration) {
        if (std::find(Input::inputButtons.begin(), Input::inputButtons.end(), kv.first) == Input::inputButtons.end()) {
            kv.second = 0;
        } else {
            kv.second++;
        }
    }

    // Insert any newly-pressed keys not yet tracked, and push to inputBuffer.
    for (const auto &key : Input::inputButtons) {
        auto durIt = Input::keyHeldDuration.find(key);
        if (durIt == Input::keyHeldDuration.end()) {
            durIt = Input::keyHeldDuration.emplace(key, 1).first;
        }

        if (key == "any" || durIt->second != 1) continue;

        Input::codePressedBlockOpcodes.clear();
        std::string addKey = (key.find(' ') == std::string::npos) ? key : key.substr(0, key.find(' '));
        std::transform(addKey.begin(), addKey.end(), addKey.begin(), ::tolower);
        Input::inputBuffer.push_back(addKey);
        if (Input::inputBuffer.size() == 101) Input::inputBuffer.erase(Input::inputBuffer.begin());
    }

    // Hot path: only iterate pre-built key-hat indices instead of scanning
    // every block on every sprite every frame.
    const std::vector<Sprite *> sprToRun = Scratch::sprites;
    for (Sprite *currentSprite : sprToRun) {
        for (Block *blockPtr : currentSprite->keyHatBlocks) {
            Block &data = *blockPtr;
            const std::string key = Scratch::getFieldValue(data, "KEY_OPTION");
            auto it = Input::keyHeldDuration.find(key);
            if (it != Input::keyHeldDuration.end() && it->second == 1) {
                if (isScriptRunning(currentSprite, data)) continue;
                executor.runBlock(data, currentSprite);
            }
        }
        for (Block *blockPtr : currentSprite->makeyKeyHatBlocks) {
            Block &data = *blockPtr;
            const std::string key = Input::convertToKey(Scratch::getInputValue(data, "KEY", currentSprite), true);
            auto it = Input::keyHeldDuration.find(key);
            if (it != Input::keyHeldDuration.end() && it->second > 0)
                executor.runBlock(data, currentSprite);
        }
    }
    BlockExecutor::runAllBlocksByOpcode("makeymakey_whenCodePressed");
}

void BlockExecutor::doSpriteClicking() {
    if (Input::mousePointer.isPressed) {
        Input::mousePointer.heldFrames++;
        bool hasClicked = false;
        for (auto &sprite : Scratch::sprites) {
            if (!sprite->visible || sprite->ghostEffect == 100.0) continue;

            // click a sprite
            if (sprite->shouldDoSpriteClick) {
                if (Input::mousePointer.heldFrames < 2 && Scratch::isColliding("mouse", sprite)) {

                    // run all "when this sprite clicked" blocks in the sprite
                    hasClicked = true;
                    for (auto &data : sprite->blocks) {
                        if (data.opcode == "event_whenthisspriteclicked" || data.opcode == "event_whenstageclicked") {
                            restartScript(sprite, data);
                            executor.runBlock(data, sprite);
                        }
                    }
                }
            }
            // start dragging a sprite
            if (Input::draggingSprite == nullptr && Input::mousePointer.heldFrames < 2 && sprite->draggable && Scratch::isColliding("mouse", sprite)) {
                Input::draggingSprite = sprite;
                dragPositionOffsetX = Input::mousePointer.x - sprite->xPosition;
                dragPositionOffsetY = Input::mousePointer.y - sprite->yPosition;
            }
            if (hasClicked) break;
        }
    } else {
        Input::mousePointer.heldFrames = 0;
    }

    // move a dragging sprite
    if (Input::draggingSprite == nullptr) return;

    if (Input::mousePointer.heldFrames == 0) {
        Input::draggingSprite = nullptr;
        return;
    }
    Input::draggingSprite->xPosition = Input::mousePointer.x - dragPositionOffsetX;
    Input::draggingSprite->yPosition = Input::mousePointer.y - dragPositionOffsetY;
}

void BlockExecutor::runRepeatBlocks() {
    bool withoutRefresh = false;

    // repeat ONLY the block most recently added to the repeat chain,,,
    std::vector<Sprite *> sprToRun = Scratch::sprites;
    for (auto &sprite : sprToRun) {
        for (auto &[id, blockChain] : sprite->blockChains) {
            const auto &repeatList = blockChain.blocksToRepeat;
            if (repeatList.empty()) continue;

            Block *const toRun = repeatList.back();
            if (toRun != nullptr) executor.runBlock(*toRun, sprite, &withoutRefresh, true);
        }
    }
    // delete sprites ready for deletion
    SpeechManager *speechManager = Render::getSpeechManager();
    for (auto *&spr : Scratch::sprites) {
        if (!spr->toDelete) continue;

        if (speechManager) {
            speechManager->clearSpeech(spr);
        }
        Scratch::cloneQueue.erase(
            std::remove(Scratch::cloneQueue.begin(), Scratch::cloneQueue.end(), spr),
            Scratch::cloneQueue.end());
        delete spr;
        spr = nullptr;
    }

    Scratch::sprites.erase(std::remove(Scratch::sprites.begin(), Scratch::sprites.end(), nullptr), Scratch::sprites.end());

    for (unsigned int i = 0; i < Scratch::sprites.size(); i++) {
        Scratch::sprites[i]->layer = (Scratch::sprites.size() - 1) - i;
    }
}

void BlockExecutor::runRepeatsWithoutRefresh(Sprite *sprite, std::string blockChainID) {
    bool withoutRefresh = true;
    if (sprite->blockChains.find(blockChainID) == sprite->blockChains.end()) return;

    while (!sprite->blockChains[blockChainID].blocksToRepeat.empty() && !sprite->toDelete) {
        Block *toRun = sprite->blockChains[blockChainID].blocksToRepeat.back();
        if (toRun != nullptr)
            executor.runBlock(*toRun, sprite, &withoutRefresh, true);
    }
}

BlockResult BlockExecutor::runCustomBlock(Sprite *sprite, Block &block, Block *callerBlock, bool *withoutScreenRefresh) {
    auto cbIt = sprite->customBlocks.find(block.customBlockId);
    if (cbIt != sprite->customBlocks.end()) {
        auto &data = cbIt->second;
        do {
            // Set up argument values
            for (std::string arg : data.argumentIds) {
                auto inputIt = block.parsedInputs->find(arg);
                if (inputIt == block.parsedInputs->end()) {
                    data.argumentValues[arg] = Value(0);
                    data.argumentBlockIds.erase(arg);
                } else {
                    data.argumentValues[arg] = Scratch::getInputValue(block, arg, sprite);
                    if (inputIt->second.inputType == ParsedInput::BLOCK) {
                        data.argumentBlockIds[arg] = inputIt->second.ref;
                    } else {
                        data.argumentBlockIds.erase(arg);
                    }
                }
            }

            // Get the parent of the prototype block (the definition containing all blocks)
            auto defIt = sprite->customBlockDefinitions.find(data.blockId);
            if (defIt == sprite->customBlockDefinitions.end()) {
                Log::logWarning("Custom block def not found: " + data.blockId);
                break;
            }
            auto blkIt = sprite->blocksMap.find(defIt->second);
            if (blkIt == sprite->blocksMap.end() || !blkIt->second) {
                Log::logWarning("Custom block map miss: " + defIt->second);
                break;
            }
            Block *customBlockDefinition = blkIt->second;

            callerBlock->customBlockPtr = customBlockDefinition;

            bool localWithoutRefresh = data.runWithoutScreenRefresh;

            // If the parent chain is running without refresh, force this one to also run without refresh
            if (!localWithoutRefresh && withoutScreenRefresh != nullptr) localWithoutRefresh = *withoutScreenRefresh;

            // std::cout << "RWSR = " << localWithoutRefresh << std::endl;

            // Execute the custom block definition
            executor.runBlock(*customBlockDefinition, sprite, &localWithoutRefresh, false);

            if (localWithoutRefresh && !sprite->toDelete) BlockExecutor::runRepeatsWithoutRefresh(sprite, customBlockDefinition->blockChainID);
        } while (false);
    }

    if (block.customBlockId == "\u200B\u200Blog\u200B\u200B %s") Log::log("[PROJECT] " + Scratch::getInputValue(block, "arg0", sprite).asString());
    if (block.customBlockId == "\u200B\u200Bwarn\u200B\u200B %s") Log::logWarning("[PROJECT] " + Scratch::getInputValue(block, "arg0", sprite).asString());
    if (block.customBlockId == "\u200B\u200Berror\u200B\u200B %s") Log::logError("[PROJECT] " + Scratch::getInputValue(block, "arg0", sprite).asString());
    if (block.customBlockId == "\u200B\u200Bopen\u200B\u200B %s .sb3") {
        Log::log("Open next Project with Block");
        Scratch::nextProject = true;
        Unzip::filePath = Scratch::getInputValue(block, "arg0", sprite).asString();
        if (Unzip::filePath.rfind("sd:", 0) == 0) {
            const std::string drivePrefix = OS::getFilesystemRootPrefix();
            Unzip::filePath.replace(0, 3, drivePrefix);
        } else if (Unzip::filePath.rfind("romfs:", 0) == 0) {
            const std::string drivePrefix = OS::getRomFSLocation();
            Unzip::filePath.replace(0, 6, drivePrefix);
        } else {
            Unzip::filePath = OS::getScratchFolderLocation() + Unzip::filePath;
        }

        if (Unzip::filePath.size() >= 1 && Unzip::filePath.back() == '/') {
            Unzip::filePath = Unzip::filePath.substr(0, Unzip::filePath.size() - 1);
        }
        if (!OS::fileExists(Unzip::filePath + "/project.json"))
            Unzip::filePath = Unzip::filePath + ".sb3";

        Scratch::dataNextProject = Value();
        Scratch::shouldStop = true;
        return BlockResult::RETURN;
    }
    if (block.customBlockId == "\u200B\u200Bopen\u200B\u200B %s .sb3 with data %s") {
        Log::log("Open next Project with Block and data");
        Scratch::nextProject = true;
        Unzip::filePath = Scratch::getInputValue(block, "arg0", sprite).asString();
        // if filepath contains sd:/ at the beginning and only at the beginning, replace it with sdmc:/
        if (Unzip::filePath.rfind("sd:", 0) == 0) {
            const std::string drivePrefix = OS::getFilesystemRootPrefix();
            Unzip::filePath.replace(0, 3, drivePrefix);
        } else if (Unzip::filePath.rfind("romfs:", 0) == 0) {
            const std::string drivePrefix = OS::getRomFSLocation();
            Unzip::filePath.replace(0, 6, drivePrefix);
        } else {
            Unzip::filePath = OS::getScratchFolderLocation() + Unzip::filePath;
        }
        if (Unzip::filePath.size() >= 1 && Unzip::filePath.back() == '/') {
            Unzip::filePath = Unzip::filePath.substr(0, Unzip::filePath.size() - 1);
        }
        if (!OS::fileExists(Unzip::filePath + "/project.json"))
            Unzip::filePath = Unzip::filePath + ".sb3";

        Unzip::filePath = OS::getScratchFolderLocation() + Unzip::filePath;

        Scratch::dataNextProject = Scratch::getInputValue(block, "arg1", sprite);
        Scratch::shouldStop = true;
        return BlockResult::RETURN;
    }

    return BlockResult::CONTINUE;
}

void BlockExecutor::runCloneStarts() {
    while (!Scratch::cloneQueue.empty()) {
        Sprite *cloningSprite = Scratch::cloneQueue.front();
        Scratch::cloneQueue.erase(Scratch::cloneQueue.begin());
        for (Sprite *sprite : Scratch::sprites) {
            if (cloningSprite != sprite) continue;
            for (auto &data : cloningSprite->blocks) {
                if (data.opcode == "control_start_as_clone") executor.runBlock(data, sprite);
            }
        }
    }
}

void BlockExecutor::runBroadcasts() {
    while (!Scratch::broadcastQueue.empty()) {
        std::string currentBroadcast = Scratch::broadcastQueue.front();
        Scratch::broadcastQueue.erase(Scratch::broadcastQueue.begin());
        std::transform(currentBroadcast.begin(), currentBroadcast.end(), currentBroadcast.begin(), ::tolower);
        runBroadcast(currentBroadcast);
    }
}

void BlockExecutor::runBroadcast(std::string broadcastToRun) {
    // Snapshot sprite list — restartScript/runBlock can mutate Scratch::sprites
    // (clones spawned/deleted during the dispatch).
    std::vector<Sprite *> sprToRun = Scratch::sprites;
    for (auto *currentSprite : sprToRun) {
        auto it = currentSprite->broadcastHatBlocks.find(broadcastToRun);
        if (it == currentSprite->broadcastHatBlocks.end()) continue;
        for (Block *blockPtr : it->second) {
            restartScript(currentSprite, *blockPtr);
            executor.runBlock(*blockPtr, currentSprite);
        }
    }
}

void BlockExecutor::runBackdrops() {
    while (!Scratch::backdropQueue.empty()) {
        const std::string currentBackdrop = Scratch::backdropQueue.front();
        Scratch::backdropQueue.erase(Scratch::backdropQueue.begin());
        runBackdrop(currentBackdrop);
    }
}

void BlockExecutor::runBackdrop(std::string backdropToRun) {
    std::vector<Sprite *> sprToRun = Scratch::sprites;
    for (auto *currentSprite : sprToRun) {
        auto it = currentSprite->backdropHatBlocks.find(backdropToRun);
        if (it == currentSprite->backdropHatBlocks.end()) continue;
        for (Block *blockPtr : it->second) {
            restartScript(currentSprite, *blockPtr);
            executor.runBlock(*blockPtr, currentSprite);
        }
    }
}

void BlockExecutor::runAllBlocksByOpcode(std::string opcodeToFind) {
    // std::cout << "Running all " << opcodeToFind << " blocks." << "\n";
    std::vector<Sprite *> sprToRun = Scratch::sprites;
    for (Sprite *currentSprite : sprToRun) {
        for (auto &data : currentSprite->blocks) {
            if (data.opcode != opcodeToFind) continue;
            if (isScriptRunning(currentSprite, data)) continue;
            executor.runBlock(data, currentSprite);
        }
    }
}

Value BlockExecutor::getBlockValue(Block &block, Sprite *sprite) {
#ifdef ENABLE_CACHING
    if (block.valueHandler != nullptr) return block.valueHandler(block, sprite);
    else {
#endif
        const auto &vh = getValueHandlers();
        const auto &it = vh.find(block.opcode);
        if (it != vh.end()) return it->second(block, sprite);
#ifdef ENABLE_CACHING
    }
#endif

    Log::logWarning("Unknown block: " + block.opcode);

    return Value();
}

void BlockExecutor::setVariableValue(const std::string &variableId, const Value &newValue, Sprite *sprite, Block *block) {
#ifdef ENABLE_CACHING
    if (block != nullptr && block->variable != nullptr) {
        block->variable->value = newValue;
#ifdef ENABLE_CLOUDVARS
        if (block->variable->cloud) cloudConnection->set(block->variable->name, block->variable->value.asString());
#endif
        return;
    }
#endif

    // Set sprite variable
    const auto it = sprite->variables.find(variableId);
    if (it != sprite->variables.end()) {
#ifdef ENABLE_CACHING
        if (block != nullptr && block->variable == nullptr) block->variable = &it->second;
#endif
        it->second.value = newValue;
        return;
    }

    auto globalIt = Scratch::stageSprite->variables.find(variableId);
    if (globalIt != Scratch::stageSprite->variables.end()) {
#ifdef ENABLE_CACHING
        if (block != nullptr && block->variable == nullptr) block->variable = &globalIt->second;
#endif
        globalIt->second.value = newValue;
#ifdef ENABLE_CLOUDVARS
        if (globalIt->second.cloud) cloudConnection->set(globalIt->second.name, globalIt->second.value.asString());
#endif
        return;
    }
}

void BlockExecutor::updateMonitors() {
    for (auto &[id, var] : Render::monitors) {
        if (var.visible) {
            Sprite *sprite = nullptr;
            for (auto &spr : Scratch::sprites) {
                if (var.spriteName == "" && spr->isStage) {
                    sprite = spr;
                    break;
                }
                if (spr->name == var.spriteName && !spr->isClone) {
                    sprite = spr;
                    break;
                }
            }

            if (var.opcode == "data_variable") {
#ifdef ENABLE_CACHING
                if (var.variablePtr != nullptr) var.value = var.variablePtr->value;
                else var.value = BlockExecutor::getVariableValue(var.id, sprite);
#else
                var.value = BlockExecutor::getVariableValue(var.id, sprite);
#endif

                var.displayName = Math::removeQuotations(var.parameters["VARIABLE"]);
                if (!sprite->isStage) var.displayName = sprite->name + ": " + var.displayName;
            } else if (var.opcode == "data_listcontents") {
                var.displayName = Math::removeQuotations(var.parameters["LIST"]);
                if (!sprite->isStage) var.displayName = sprite->name + ": " + var.displayName;

#ifdef ENABLE_CACHING
                if (var.listPtr != nullptr) {
                    var.list = var.listPtr->items;
                } else {
#endif
                    // Check lists
                    auto listIt = sprite->lists.find(var.id);
                    if (listIt != sprite->lists.end()) {
                        var.list = listIt->second.items;
#ifdef ENABLE_CACHING
                        var.listPtr = &listIt->second;
#endif
                    }

                    // Check global lists
                    auto globalIt = Scratch::stageSprite->lists.find(var.id);
                    if (globalIt != Scratch::stageSprite->lists.end()) {
                        var.list = globalIt->second.items;
#ifdef ENABLE_CACHING
                        var.listPtr = &globalIt->second;
#endif
                    }
#ifdef ENABLE_CACHING
                }
#endif
            } else {
                Block newBlock;
                newBlock.opcode = var.opcode;
                for (const auto &[paramName, paramValue] : var.parameters) {
                    ParsedField parsedField;
                    parsedField.value = Math::removeQuotations(paramValue);
                    (*newBlock.parsedFields)[paramName] = parsedField;
                }
                if (var.opcode == "looks_costumenumbername")
                    var.displayName = var.spriteName + ": costume " + Scratch::getFieldValue(newBlock, "NUMBER_NAME");
                else if (var.opcode == "looks_backdropnumbername")
                    var.displayName = "backdrop " + Scratch::getFieldValue(newBlock, "NUMBER_NAME");
                else if (var.opcode == "sensing_current")
                    var.displayName = std::string(MonitorDisplayNames::getCurrentMenuMonitorName(Scratch::getFieldValue(newBlock, "CURRENTMENU")));
                else {
                    auto spriteName = MonitorDisplayNames::getSpriteMonitorName(var.opcode);
                    if (spriteName != var.opcode) {
                        var.displayName = var.spriteName + ": " + std::string(spriteName);
                    } else {
                        auto simpleName = MonitorDisplayNames::getSimpleMonitorName(var.opcode);
                        var.displayName = simpleName != var.opcode ? std::string(simpleName) : var.opcode;
                    }
                }
                var.value = executor.getBlockValue(newBlock, sprite);
            }
        }
    }
}

Value BlockExecutor::getVariableValue(const std::string &variableId, Sprite *sprite, Block *block) {
#ifdef ENABLE_CACHING
    if (block != nullptr && block->variable != nullptr) return block->variable->value;
#endif

    // Check sprite variables
    const auto it = sprite->variables.find(variableId);
    if (it != sprite->variables.end()) {
#ifdef ENABLE_CACHING
        if (block != nullptr && block->variable == nullptr) block->variable = &it->second;
#endif
        return it->second.value;
    }

    // Check lists
    const auto listIt = sprite->lists.find(variableId);
    if (listIt != sprite->lists.end()) {
        std::string result;
        std::string seperator = "";
        for (const auto &item : listIt->second.items) {
            if (item.asString().size() > 1 || !item.isString()) {
                seperator = " ";
                break;
            }
        }
        for (const auto &item : listIt->second.items) {
            result += item.asString() + seperator;
        }
        if (!result.empty() && !seperator.empty()) result.pop_back();
        return Value(result);
    }

    // Check global variables
    const auto globalIt = Scratch::stageSprite->variables.find(variableId);
    if (globalIt != Scratch::stageSprite->variables.end()) {
#ifdef ENABLE_CACHING
        if (block != nullptr && block->variable == nullptr) block->variable = &globalIt->second;
#endif
        return globalIt->second.value;
    }

    // Check global lists
    auto globalListIt = Scratch::stageSprite->lists.find(variableId);
    if (globalListIt != Scratch::stageSprite->lists.end()) {
        std::string result;
        std::string seperator = "";
        for (const auto &item : globalListIt->second.items) {
            if (item.asString().size() > 1 || !item.isString()) {
                seperator = " ";
                break;
            }
        }
        for (const auto &item : globalListIt->second.items) {
            result += item.asString() + seperator;
        }
        if (!result.empty() && !seperator.empty()) result.pop_back();
        return Value(result);
    }

    return Value();
}

#ifdef ENABLE_CLOUDVARS
void BlockExecutor::handleCloudVariableChange(const std::string &name, const std::string &value) {
    for (const auto &currentSprite : Scratch::sprites) {
        if (currentSprite->isStage) {
            for (auto it = currentSprite->variables.begin(); it != currentSprite->variables.end(); ++it) {
                if (it->second.name != name) continue;
                it->second.value = Value(value);
                return;
            }
        }
    }
}
#endif

Value BlockExecutor::getCustomBlockValue(std::string valueName, Sprite *sprite, Block block) {
    // get the parent prototype block
    Block *const definitionBlock = Scratch::getBlockParent(&block, sprite);
    const Block *prototypeBlock = Scratch::findBlock(Scratch::getInputValue(*definitionBlock, "custom_block", sprite).asString(), sprite);

    for (auto &[custId, custBlock] : sprite->customBlocks) {
        // variable must be in the same custom block
        if (prototypeBlock != nullptr && custBlock.blockId != prototypeBlock->id) continue;

        size_t index = custBlock.argumentNames.size();
        for (size_t i = custBlock.argumentNames.size(); i-- > 0;) {
            if (custBlock.argumentNames[i] == valueName) {
                index = i;
                break;
            }
        }

        if (index == custBlock.argumentNames.size()) {
            continue;
        }

        if (index < custBlock.argumentIds.size()) {
            const std::string argumentId = custBlock.argumentIds[index];

            const auto valueIt = custBlock.argumentValues.find(argumentId);
            if (valueIt != custBlock.argumentValues.end()) {
                return valueIt->second;
                continue;
            }

            Log::logWarning("Argument ID found, but no value exists for it.");
            continue;
        }
        Log::logWarning("Index out of bounds for argumentIds!");
    }
    return Value();
}

Value BlockExecutor::getCustomBlockBooleanValue(std::string valueName, Sprite *sprite, Block block) {
    Block *const definitionBlock = Scratch::getBlockParent(&block, sprite);
    const Block *prototypeBlock = Scratch::findBlock(Scratch::getInputValue(*definitionBlock, "custom_block", sprite).asString(), sprite);

    for (auto &[custId, custBlock] : sprite->customBlocks) {
        if (prototypeBlock != nullptr && custBlock.blockId != prototypeBlock->id) continue;

        size_t index = custBlock.argumentNames.size();
        for (size_t i = custBlock.argumentNames.size(); i-- > 0;) {
            if (custBlock.argumentNames[i] == valueName) {
                index = i;
                break;
            }
        }
        if (index == custBlock.argumentNames.size()) continue;
        if (index >= custBlock.argumentIds.size()) continue;

        const std::string &argumentId = custBlock.argumentIds[index];

        // Boolean params re-evaluate the plugged-in reporter each access.
        const auto blkIt = custBlock.argumentBlockIds.find(argumentId);
        if (blkIt != custBlock.argumentBlockIds.end() && !blkIt->second.empty()) {
            Block *src = Scratch::findBlock(blkIt->second, sprite);
            if (src) return executor.getBlockValue(*src, sprite);
        }

        const auto valueIt = custBlock.argumentValues.find(argumentId);
        if (valueIt != custBlock.argumentValues.end()) return valueIt->second;
        return Value(false);
    }
    return Value(false);
}

void BlockExecutor::addToRepeatQueue(Sprite *sprite, Block *block) {
    auto &repeatList = sprite->blockChains[block->blockChainID].blocksToRepeat;
    if (std::find(repeatList.begin(), repeatList.end(), block) == repeatList.end()) {
        repeatList.push_back(block);
    }
}

void BlockExecutor::removeFromRepeatQueue(Sprite *sprite, Block *block) {
    auto it = sprite->blockChains.find(block->blockChainID);
    if (it == sprite->blockChains.end()) return;

    auto &blocksToRepeat = it->second.blocksToRepeat;
    if (!blocksToRepeat.empty()) {
        blocksToRepeat.pop_back();
    }
}

bool BlockExecutor::hasActiveRepeats(Sprite *sprite, std::string blockChainID) {
    if (sprite->toDelete) return false;
    return (sprite->blockChains.find(blockChainID) != sprite->blockChains.end() && !sprite->blockChains[blockChainID].blocksToRepeat.empty());
}
