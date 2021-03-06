#include "SpinInput.hpp"
#include "Label.hpp"
#include "Button.hpp"

#include <wx/dcgraph.h>

BEGIN_EVENT_TABLE(SpinInput, wxPanel)

EVT_MOTION(SpinInput::mouseMoved)
EVT_ENTER_WINDOW(SpinInput::mouseEnterWindow)
EVT_LEAVE_WINDOW(SpinInput::mouseLeaveWindow)
EVT_KEY_DOWN(SpinInput::keyPressed)
EVT_KEY_UP(SpinInput::keyReleased)
EVT_MOUSEWHEEL(SpinInput::mouseWheelMoved)

// catch paint events
EVT_PAINT(SpinInput::paintEvent)

END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

SpinInput::SpinInput(wxWindow *     parent,
                     wxString       text,
                     wxString       label,
                     const wxPoint &pos,
                     const wxSize & size,
                     long           style,
                     int min, int max, int initial)
    : wxWindow(parent, wxID_ANY, pos, size)
    , state_handler(this)
    , border_color(std::make_pair(0xDBDBDB, (int) StateColor::Disabled),
                   std::make_pair(0x00AE42, (int) StateColor::Focused),
                   std::make_pair(0x00AE42, (int) StateColor::Hovered),
                   std::make_pair(0xDBDBDB, (int) StateColor::Normal))
    , text_color(std::make_pair(0xACACAC, (int) StateColor::Disabled),
                 std::make_pair(*wxBLACK, (int) StateColor::Normal))
    , background_color(std::make_pair(0xF0F0F0, (int) StateColor::Disabled),
                       std::make_pair(*wxWHITE, (int) StateColor::Normal))
{
    hover = false;
    radius = 0;
    SetFont(Label::Body_12);
    wxWindow::SetLabel(label);
    state_handler.attach({&border_color, &text_color, &background_color});
    state_handler.update_binds();
    text_ctrl = new wxTextCtrl(this, wxID_ANY, text, {20, 5}, wxDefaultSize,
                               style | wxBORDER_NONE | wxTE_PROCESS_ENTER, wxTextValidator(wxFILTER_DIGITS));
    text_ctrl->SetFont(Label::Body_14);
    text_ctrl->Bind(wxEVT_SET_FOCUS, [this](auto &e) {
        e.SetId(GetId());
        ProcessEventLocally(e);
    });
    text_ctrl->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
        e.SetId(GetId());
        ProcessEventLocally(e);
    });
    text_ctrl->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
        e.SetId(GetId());
        ProcessEventLocally(e);
    });
    text_ctrl->Bind(wxEVT_KILL_FOCUS, &SpinInput::onTextLostFocus, this);
    text_ctrl->Bind(wxEVT_TEXT_ENTER, &SpinInput::onTextEnter, this);
    text_ctrl->Bind(wxEVT_RIGHT_DOWN, [this](auto &e) {}); // disable context menu
    button_inc = createButton(true);
    button_dec = createButton(false);
    delta      = 0;
    timer.Bind(wxEVT_TIMER, &SpinInput::onTimer, this);

    long initialFromText;
    if ( text.ToLong(&initialFromText) )
        initial = initialFromText;
    SetRange(min, max);
    SetValue(initial);
    messureSize();
}

void SpinInput::SetCornerRadius(double radius)
{
    this->radius = radius;
    Refresh();
}

void SpinInput::SetLabel(const wxString &label)
{
    wxWindow::SetLabel(label);
    messureSize();
    Refresh();
}

void SpinInput::SetTextColor(StateColor const &color)
{
    text_color = color;
    state_handler.update_binds();
}

void SpinInput::SetBackgroundColor(StateColor const& color)
{
    background_color = color;
    state_handler.update_binds();
}

void SpinInput::SetSize(wxSize const &size)
{
    wxWindow::SetSize(size);
    Rescale();
}

void SpinInput::SetValue(const wxString &text)
{
    long value;
    if ( text.ToLong(&value) )
        SetValue(value);
}

void SpinInput::SetValue(int value)
{
    if (value < min) value = min;
    else if (value > max) value = max;
    this->val = value;
    text_ctrl->SetValue(wxString::FromDouble(value));
}

int SpinInput::GetValue()const
{
    return val;
}

void SpinInput::SetRange(int min, int max)
{
    this->min = min;
    this->max = max;
}

void SpinInput::DoSetToolTipText(wxString const &tip)
{ 
    wxWindow::DoSetToolTipText(tip);
    text_ctrl->SetToolTip(tip);
}

void SpinInput::Rescale()
{
    button_inc->Rescale();
    button_dec->Rescale();
    messureSize();
}

bool SpinInput::Enable(bool enable)
{
    bool result = text_ctrl->Enable(enable) && wxWindow::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
    }
    return result;
}

void SpinInput::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void SpinInput::render(wxDC& dc)
{
    int    states = state_handler.states();
    wxSize size = GetSize();
    dc.SetPen(wxPen(border_color.colorForStates(states)));
    dc.SetBrush(wxBrush(background_color.colorForStates(states)));
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
    wxPoint pt = button_inc->GetPosition();
    pt.y = size.y / 2;
    dc.SetPen(wxPen(border_color.defaultColor()));
    dc.DrawLine(pt, pt + wxSize{button_inc->GetSize().x - 2, 0});
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    // start draw
    auto text = GetLabel();
    if (!text.IsEmpty()) {
        pt.x = size.x - labelSize.x - 5;
        pt.y = (size.y - labelSize.y) / 2;
        dc.SetFont(GetFont());
        dc.SetTextForeground(text_color.colorForStates(states));
        dc.DrawText(text, pt);
    }
}

void SpinInput::messureSize()
{
    wxSize size = GetSize();
    wxSize textSize = text_ctrl->GetSize();
#ifdef __WXOSX__
    textSize.y -= 3; // TODO:
#endif
    int h = textSize.y * 24 / 14;
    if (size.y < h) {
        size.y = h;
        SetSize(size);
        SetMinSize(size);
    } else {
        textSize.y = size.y * 14 / 24;
    }
    wxSize btnSize = {14, (size.y - 4) / 2};
    btnSize.x = btnSize.x * btnSize.y / 10;
    wxClientDC dc(this);
    labelSize  = dc.GetMultiLineTextExtent(GetLabel());
    textSize.x = size.x - labelSize.x - btnSize.x - 16;
    text_ctrl->SetSize(textSize);
    text_ctrl->SetPosition({6 + btnSize.x, (size.y - textSize.y) / 2});
    button_inc->SetSize(btnSize);
    button_dec->SetSize(btnSize);
    button_inc->SetPosition({3, size.y / 2 - btnSize.y - 1});
    button_dec->SetPosition({3, size.y / 2 + 1});
}

Button *SpinInput::createButton(bool inc)
{
    auto btn = new Button(this, "", inc ? "spin_inc" : "spin_dec", wxBORDER_NONE, 6);
    btn->SetCornerRadius(0);
    btn->SetCanFocus(false);
    btn->Bind(wxEVT_LEFT_DOWN, [=](auto &e) {
        delta = inc ? 1 : -1;
        SetValue(val + delta);
        text_ctrl->SetFocus();
        btn->CaptureMouse();
        delta *= 8;
        timer.Start(100);
        sendSpinEvent();
    });
    btn->Bind(wxEVT_LEFT_DCLICK, [=](auto &e) {
        delta = inc ? 1 : -1;
        SetValue(val + delta);
        sendSpinEvent();
    });
    btn->Bind(wxEVT_LEFT_UP, [=](auto &e) {
        btn->ReleaseMouse();
        timer.Stop();
        text_ctrl->SelectAll();
        delta = 0;
    });
    return btn;
}

void SpinInput::mouseEnterWindow(wxMouseEvent& event)
{
    if (!hover)
    {
        hover = true;
        Refresh();
    }
}

void SpinInput::mouseLeaveWindow(wxMouseEvent& event)
{
    if (hover)
    {
        hover = false;
        Refresh();
    }
}

void SpinInput::onTimer(wxTimerEvent &evnet) {
    if (delta < -1 || delta > 1) {
        delta /= 2;
        return;
    }
    SetValue(val + delta);
    sendSpinEvent();
}

void SpinInput::onTextLostFocus(wxEvent &event)
{
    timer.Stop();
    for (auto * child : GetChildren())
        if (auto btn = dynamic_cast<Button*>(child))
            if (btn->HasCapture())
                btn->ReleaseMouse();
    wxCommandEvent e;
    onTextEnter(e);
    // pass to outer
    event.SetId(GetId());
    ProcessEventLocally(event);
}

void SpinInput::onTextEnter(wxCommandEvent &event)
{
    long value;
    if (!text_ctrl->GetValue().ToLong(&value)) { value = val; }
    if (value != val) {
        SetValue(value);
        sendSpinEvent();
    }
    event.SetId(GetId());
    ProcessEventLocally(event);
}

void SpinInput::mouseWheelMoved(wxMouseEvent &event)
{
    auto delta = (event.GetWheelRotation() < 0 == event.IsWheelInverted()) ? 1 : -1;
    SetValue(val + delta);
    sendSpinEvent();
    text_ctrl->SetFocus();
}

// currently unused events
void SpinInput::mouseMoved(wxMouseEvent& event) {}
void SpinInput::keyPressed(wxKeyEvent& event) {}
void SpinInput::keyReleased(wxKeyEvent &event) {}

void SpinInput::sendSpinEvent()
{
    wxCommandEvent event(wxEVT_SPINCTRL, GetId());
    event.SetEventObject(this);
    GetEventHandler()->ProcessEvent(event); 
}
