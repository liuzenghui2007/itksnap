#include "ReorientImageModel.h"
#include "GlobalUIModel.h"
#include "IRISException.h"
#include "IRISApplication.h"
#include "GenericImageData.h"
#include "GenericSliceModel.h"




ReorientImageModel::ReorientImageModel()
{
  // Initialized cached values
  m_CurrentRAIValue = "";
  m_CurrentOrientationIsOblique = false;

  // Create the sub-models
  m_NewRAICodeModel = ConcreteSimpleStringProperty::New();
  m_NewRAICodeModel->SetValue("");

  m_CurrentRAICodeModel = wrapGetterSetterPairAsProperty(
        this,
        &Self::GetCurrentRAICodeValue);

  m_InvalidStatusModel = wrapGetterSetterPairAsProperty(
        this,
        &Self::GetInvalidStatusValue);

  // Invalid status model listens to changes to the new RAI model
  m_InvalidStatusModel->Rebroadcast(m_NewRAICodeModel,
                                    ValueChangedEvent(), ValueChangedEvent());

  // Changes to the new RAI model are rebroadcast as state change events
  // for the widget activation system
  Rebroadcast(m_NewRAICodeModel, ValueChangedEvent(),
              StateMachineChangeEvent());

  // Create the axis direction models
  for(int axis = 0; axis < 3; axis++)
    {
    m_NewAxisDirectionModel[axis] = wrapIndexedGetterSetterPairAsProperty(
          this, axis,
          &Self::GetNthNewAxisDirectionValueAndDomain,
          &Self::SetNthNewAxisDirectionValue);

    m_NewAxisDirectionModel[axis]->Rebroadcast(
          m_NewRAICodeModel, ValueChangedEvent(), ValueChangedEvent());

    m_NewAxisDirectionModel[axis]->Rebroadcast(
          m_NewRAICodeModel, ValueChangedEvent(), DomainChangedEvent());

    m_CurrentAxisDirectionModel[axis] = wrapIndexedGetterSetterPairAsProperty(
          this, axis,
          &Self::GetNthCurrentAxisDirectionValue);
    }
}

/** Initialize with the parent model */
void ReorientImageModel::SetParentModel(GlobalUIModel *parent)
{
  // Store the model
  m_Parent = parent;

  // Listen to changes to the main image
  Rebroadcast(m_Parent->GetDriver(),
              MainImageDimensionsChangeEvent(), ModelUpdateEvent());

  // Listen to changes to the main image
  Rebroadcast(m_Parent->GetDriver(),
              MainImagePoseChangeEvent(), ModelUpdateEvent());
}

AbstractSimpleStringProperty
*ReorientImageModel::GetCurrentAxisDirectionModel(int axis) const
{
  return m_CurrentAxisDirectionModel[axis];
}

ReorientImageModel::AbstractAxisDirectionProperty
*ReorientImageModel::GetNewAxisDirectionModel(int axis) const
{
  return m_NewAxisDirectionModel[axis];
}

void ReorientImageModel::ApplyCurrentRAI()
{
  IRISApplication *driver = m_Parent->GetDriver();

  // Check that the current RAI is valid
  std::string rai = this->m_NewRAICodeModel->GetValue();
  assert(ImageCoordinateGeometry::IsRAICodeValid(rai.c_str()));

  // Convert the rai code to a direction matrix
  ImageCoordinateGeometry::DirectionMatrix dm =
      ImageCoordinateGeometry::ConvertRAICodeToDirectionMatrix(rai);

  // Set the direction in the image
  driver->ReorientImage(dm);

  // Tell the display slices to reinitialize
  for(int i = 0; i < 3; i++)
    {
    m_Parent->GetSliceModel(i)->InitializeSlice(driver->GetCurrentImageData());
    }
}

void ReorientImageModel::ReverseAxisDirection(int axis)
{
  AxisDirection dir;
  bool valid = m_NewAxisDirectionModel[axis]->GetValueAndDomain(dir, NULL);
  if(valid)
    {
    AxisDirection reverse =
        static_cast<AxisDirection>(- static_cast<int>(dir));
    m_NewAxisDirectionModel[axis]->SetValue(reverse);
    }
}

bool ReorientImageModel::CheckState(ReorientImageModel::UIState state)
{
  AxisDirection dummy;
  switch(state)
    {
    case UIF_VALID_NEW_RAI:
      {
      std::string rai = this->m_NewRAICodeModel->GetValue();
      return ImageCoordinateGeometry::IsRAICodeValid(rai.c_str());
      }
    case UIF_VALID_AXIS_DIRECTION_X:
      return GetNthNewAxisDirectionValueAndDomain(0, dummy, NULL);
    case UIF_VALID_AXIS_DIRECTION_Y:
      return GetNthNewAxisDirectionValueAndDomain(1, dummy, NULL);
    case UIF_VALID_AXIS_DIRECTION_Z:
      return GetNthNewAxisDirectionValueAndDomain(2, dummy, NULL);
    default: return false;
    }
}


bool ReorientImageModel::GetCurrentRAICodeValue(std::string &value)
{
  if(m_CurrentRAIValue.size())
    {
    value = m_CurrentOrientationIsOblique
        ? std::string("Oblique, closest to ") + m_CurrentRAIValue
        : m_CurrentRAIValue;
    return true;
    }
  else
    return false;
}

bool ReorientImageModel
::GetNthCurrentAxisDirectionValue(int axis, std::string &value)
{
  // Get the current rai code value
  if(m_CurrentRAIValue.size())
    {
    char letter = m_CurrentRAIValue[axis];
    AxisDirection dir =
        ImageCoordinateGeometry::ConvertRAILetterToAxisDirection(letter);
    std::string dirstr
        = ImageCoordinateGeometry::GetAxisDirectionDescriptionMap()[dir];
    value = m_CurrentOrientationIsOblique
        ? std::string("Oblique, closest to ") + dirstr
        : dirstr;
    return true;
    }
  else
    return false;
}


bool ReorientImageModel::GetInvalidStatusValue(std::string &value)
{
  // Check that the current RAI is valid
  std::string rai = this->m_NewRAICodeModel->GetValue();
  if(ImageCoordinateGeometry::IsRAICodeValid(rai.c_str()))
    value = "";
  else
    value = "Invalid RAI code";
  return true;
}


bool ReorientImageModel
::GetNthNewAxisDirectionValueAndDomain(
    int axis,
    AxisDirection &value,
    AxisDirectionDomain *domain)
{
  // Check that the current RAI is 'sort of' valid
  std::string rai = this->m_NewRAICodeModel->GetValue();

  // Make sure that the rai is long enough for us to process
  if(rai.size() <= axis)
    return false;

  // Make sure that the letter in the RAI is a valid letter
  value = ImageCoordinateGeometry::ConvertRAILetterToAxisDirection(rai[axis]);

  // Is this a valid direction?
  if(value == ImageCoordinateGeometry::INVALID_DIRECTION)
    return false;

  // Now deal with the domain
  if(domain)
    {
    domain->SetWrappedMap(
          &ImageCoordinateGeometry::GetAxisDirectionDescriptionMap());
    }

  return true;
}

void ReorientImageModel
::SetNthNewAxisDirectionValue(int axis, AxisDirection value)
{
  // Get the letter for the direction
  char letter = ImageCoordinateGeometry::ConvertAxisDirectionToRAILetter(value);

  // Make sure the RAI code is long enough for the letter to fit
  std::string rai = this->m_NewRAICodeModel->GetValue();
  while(rai.size() <= axis)
    rai.append(" ");

  // Set the letter in the RAI code
  rai[axis] = letter;

  // Set the rai string in the model
  m_NewRAICodeModel->SetValue(rai);
}

void ReorientImageModel::OnUpdate()
{
  if(this->m_EventBucket->HasEvent(MainImageDimensionsChangeEvent())
     || this->m_EventBucket->HasEvent(MainImagePoseChangeEvent()))
    {    
    // Obtain the current RAI value
    IRISApplication *app = m_Parent->GetDriver();
    if(app->GetCurrentImageData()->IsMainLoaded())
      {
      m_CurrentRAIValue = app->GetImageToAnatomyRAI();
      m_CurrentOrientationIsOblique = app->IsImageOrientationOblique();
      }
    else
      {
      m_CurrentRAIValue = "";
      }

    // Set it as the new RAI value
    m_NewRAICodeModel->SetValue(m_CurrentRAIValue);
    }
}


