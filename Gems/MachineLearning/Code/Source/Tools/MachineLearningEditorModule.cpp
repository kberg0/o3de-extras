
#include <MachineLearning/MachineLearningTypeIds.h>
#include <MachineLearningModuleInterface.h>
#include "MachineLearningEditorSystemComponent.h"

namespace MachineLearning
{
    class MachineLearningEditorModule
        : public MachineLearningModuleInterface
    {
    public:
        AZ_RTTI(MachineLearningEditorModule, MachineLearningEditorModuleTypeId, MachineLearningModuleInterface);
        AZ_CLASS_ALLOCATOR(MachineLearningEditorModule, AZ::SystemAllocator);

        MachineLearningEditorModule()
        {
            // Push results of [MyComponent]::CreateDescriptor() into m_descriptors here.
            // Add ALL components descriptors associated with this gem to m_descriptors.
            // This will associate the AzTypeInfo information for the components with the the SerializeContext, BehaviorContext and EditContext.
            // This happens through the [MyComponent]::Reflect() function.
            m_descriptors.insert(m_descriptors.end(), {
                MachineLearningEditorSystemComponent::CreateDescriptor(),
            });
        }

        /**
         * Add required SystemComponents to the SystemEntity.
         * Non-SystemComponents should not be added here
         */
        AZ::ComponentTypeList GetRequiredSystemComponents() const override
        {
            return AZ::ComponentTypeList {
                azrtti_typeid<MachineLearningEditorSystemComponent>(),
            };
        }
    };
}// namespace MachineLearning

AZ_DECLARE_MODULE_CLASS(Gem_MachineLearning, MachineLearning::MachineLearningEditorModule)
