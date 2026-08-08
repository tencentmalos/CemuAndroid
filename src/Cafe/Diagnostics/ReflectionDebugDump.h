#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "spatial/reflection/RegisterMacro.hpp"
#include "spatial/reflection/objects/MakeUserObject.hpp"

namespace ReflectionDebugDump
{
	template<typename Payload>
	[[nodiscard]] std::string PackScalarObject(const Payload& payload)
	{
		const spatial::reflection::UserObject object = spatial::reflection::make_external_user_object(payload);
		std::ostringstream out;
		for (const spatial::reflection::Property* property : object.GetClass().GetPropertiesOrderByTag())
		{
			if (property == nullptr || !property->IsReadable())
				continue;
			const spatial::reflection::AnyValue value = property->GetField(object);
			out << property->name() << '=';
			switch (property->kind())
			{
			case spatial::reflection::ValueKind::kBoolean:
				out << (value.get<bool>() ? "true" : "false");
				break;
			case spatial::reflection::ValueKind::kReal:
				out << value.get<double>();
				break;
			case spatial::reflection::ValueKind::kString:
				out << value.get<std::string>();
				break;
			case spatial::reflection::ValueKind::kStringView:
				out << value.get<std::string_view>();
				break;
			case spatial::reflection::ValueKind::kEnum:
				out << static_cast<std::uint64_t>(std::get<spatial::reflection::EnumObject>(value).GetValue());
				break;
			case spatial::reflection::ValueKind::kInteger:
			default:
				out << value.get<std::uint64_t>();
				break;
			}
			out << '\n';
		}
		return out.str();
	}
}
